# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute pipeline selection and per-frame accounting without a graphics driver."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ShaderProbeTests(unittest.TestCase):
    def test_selection_and_sampled_frames(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "probe.cpp"
            executable = Path(directory) / "probe"
            source.write_text(r"""
#include <cassert>
#include <limits>
#include "common/nxbox_shader_probe.h"
using namespace NxboxShaderProbe;
int main() {
    Selection selection;
    assert(selection.Parse(" 0x10 , 20-2f, FFFFFFFFFFFFFFFF "));
    assert(selection.Contains(0x10) && selection.Contains(0x2a));
    assert(selection.Contains(std::numeric_limits<Hash>::max()));
    assert(!selection.Contains(0x11) && !selection.Contains(0x30));
    for (const auto bad : {"-1", "30-20", "1,", ",1", "gg", "1junk",
                           "10000000000000000", "1-2-3", "1,,2"}) {
        Selection invalid;
        assert(!invalid.Parse(bad) && invalid.ranges.empty());
    }
    assert(!Selection{}.Parse("1-2", false));
    assert(Selection{}.Parse(""));
    Bucket bucket;
    for (const auto bad : {"1:2/2", "1:0/0", "-1:0/2", "0:0/2junk", "0/2", "0:0"}) {
        assert(!bucket.Parse(bad) && bucket.count == 0);
    }
    assert(bucket.Parse("abc:1/4"));
    for (Hash hash = 0; hash < 10000; ++hash) {
        unsigned matches = 0;
        for (unsigned k = 0; k < 4; ++k) {
            Bucket part;
            assert(part.Parse("abc:" + std::to_string(k) + "/4"));
            matches += part.Contains(hash);
        }
        assert(matches == 1);
        assert(bucket.Contains(hash) == (((hash ^ 0xabc) % 4) == 1));
    }
    Config config;
    Frames frames;
    const std::array<Hash, 6> stages{0x11, 0x22, 0, 0, 0, 0x33};
    assert(!config.DrawEnabled());
    frames.Record(config, 7, stages, false);
    assert(frames.pipelines.empty());
    assert(config.dump.Parse("22"));
    assert(config.Dump(7, stages) && !config.Dump(7, {}));
    assert(!config.DrawEnabled()); // dump alone never instruments draws
    assert(config.dump.Parse("7") && config.Dump(7, {}));
    assert(config.skip.Parse("7-9"));
    assert(config.Skip(8) && !config.Skip(10));
    config.bucket = bucket;
    assert(config.Skip(9) || bucket.Contains(9));
    config.ids = true;
    config.every = 2;
    frames.Record(config, 7, stages, false);
    frames.Record(config, 7, stages, false);
    frames.Record(config, 7, stages, true);
    frames.Record(config, 8, stages, true);
    std::vector<std::string> lines;
    auto log = [&](const std::string& line) { lines.push_back(line); };
    frames.End(config, log);
    assert(lines.size() == 3 && frames.pipelines.empty());
    assert(lines[0] == "SHADER_IDS frame=0 pipelines=2");
    assert(lines[1].find("p=0000000000000007 d=2 skip=1") != std::string::npos);
    assert(lines[1].find("s=0000000000000011,0000000000000022,-,-,-,0000000000000033") != std::string::npos);
    frames.Record(config, 9, stages, false);
    frames.End(config, log);
    assert(lines.size() == 3); // unsampled frame neither allocates nor leaks counts
    frames.Record(config, 10, stages, false);
    frames.End(config, log);
    assert(lines.size() == 5 && lines.back().find("d=1 skip=0") != std::string::npos);
    frames.End(config, log);
    frames.End(config, log);
    assert(lines.back() == "SHADER_IDS frame=4 pipelines=0");
}
""")
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)

    def test_graphics_hooks_cover_indirect_and_disk_cache(self):
        rasterizer = (ROOT / "src/video_core/renderer_opengl/gl_rasterizer.cpp").read_text()
        prepare = rasterizer.split("void RasterizerOpenGL::PrepareDraw", 1)[1].split(
            "void RasterizerOpenGL::Draw(", 1
        )[0]
        self.assertLess(prepare.index("pipeline->Configure"), prepare.index("config.Skip"))
        self.assertLess(prepare.index("config.Skip"), prepare.index("BeginTransformFeedback"))
        self.assertLess(prepare.index("frames.Record"), prepare.index("draw_func(primitive_mode)"))
        indirect = rasterizer.split("void RasterizerOpenGL::DrawIndirect", 1)[1].split(
            "void RasterizerOpenGL::DrawTexture", 1
        )[0]
        self.assertIn("PrepareDraw(params.is_indexed", indirect)
        self.assertIn("FrameState().End", rasterizer)
        cache = (ROOT / "src/video_core/renderer_opengl/gl_shader_cache.cpp").read_text()
        create = cache.split("std::span<Shader::Environment* const> envs", 1)[1].split(
            "std::unique_ptr<ComputePipeline>", 1
        )[0]
        self.assertLess(create.index("EmitSPIRV"), create.index("if (dump_pipeline)"))
        self.assertLess(
            create.index("if (dump_pipeline)"), create.index("std::make_unique<GraphicsPipeline>")
        )
        self.assertIn(".maxwell.bin", create)
        self.assertIn("live->GuestCodeAddress()", create)
        self.assertIn("cached->GuestCodeAddress()", create)
        self.assertIn('".spv"', create)
        self.assertIn('".glsl"', create)
        self.assertIn("CreateGraphicsPipeline(ctx->pools, key", cache)

    def test_live_maxwell_snapshot_bounds(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        implementation = (ROOT / "src/video_core/shader_environment.cpp").read_text()
        function = (
            "std::vector<u64> GenericEnvironment::GuestCode() const {"
            + implementation.split("std::vector<u64> GenericEnvironment::GuestCode() const {", 1)[
                1
            ].split("#endif", 1)[0]
        )
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "snapshot.cpp"
            executable = Path(directory) / "snapshot"
            source.write_text(
                r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr std::size_t INST_SIZE = sizeof(u64);
struct Memory {
    std::vector<u64> words{1, 2, 3, 4, 5, 6, 7, 8};
    unsigned reads{};
    void ReadBlock(u64 address, void* output, std::size_t size) {
        assert(address % 8 == 0 && address + size <= words.size() * 8);
        ++reads;
        std::memcpy(output, reinterpret_cast<const char*>(words.data()) + address, size);
    }
};
struct GenericEnvironment {
    std::vector<u64> code{101, 102, 103, 999};
    bool has_unbound_instructions{};
    u32 cached_lowest{8}, cached_highest{24}, read_lowest{16}, read_highest{24};
    u64 program_base{};
    Memory* gpu_memory{};
    std::size_t CachedSizeWords() const { return (cached_highest - cached_lowest) / 8 + 1; }
    u32 GuestCodeAddress() const { return (std::min)(cached_lowest, read_lowest); }
    std::vector<u64> GuestCode() const;
};
"""
                + function
                + r"""
int main() {
    Memory memory;
    GenericEnvironment env;
    env.gpu_memory = &memory;
    const auto cached = env.GuestCode();
    assert((cached == std::vector<u64>{101, 102, 103}));
    assert(memory.reads == 0); // exact cached bytes, no linear-scan tail
    env.has_unbound_instructions = true;
    env.read_lowest = 0;
    env.read_highest = 40;
    const auto extended = env.GuestCode();
    assert((extended == std::vector<u64>{1, 2, 3, 4, 5, 6}));
    assert(memory.reads == 1 && env.GuestCodeAddress() == 0);
    assert(env.code[0] == 101); // diagnostic capture does not alter translation input
}
"""
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)
