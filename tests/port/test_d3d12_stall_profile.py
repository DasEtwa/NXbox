# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native-index equivalence, inactive specialization and negative DXC caching."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"
DRIVER = r"""
#include <cassert>
#include <array>
#include <cstring>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_index_converter.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_dxc.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
using namespace D3D12;
namespace D3D12 {
struct ShaderCompiler::Impl {};
ShaderCompiler::ShaderCompiler() = default;
ShaderCompiler::~ShaderCompiler() = default;
static unsigned calls;
std::vector<u8> ShaderCompiler::Compile(std::string_view source, const wchar_t*, const wchar_t*, u64) {
    ++calls;
    if (source == "bad") throw std::runtime_error("synthetic DXC validation failure");
    return {1, 2, 3};
}
}
int main() {
    using namespace IndexConversion;
    // Every admitted native list is byte-for-byte equivalent to conversion.
    for (auto topology : {Topology::Points, Topology::Lines, Topology::Triangles}) {
        for (auto format : {Format::Uint16, Format::Uint32}) {
            for (auto provoking : {Provoking::First, Provoking::Preserve, Provoking::Last}) {
                if (NeedsConversion(topology, format, {}, provoking)) continue;
                for (unsigned n = 0; n < 15; ++n) {
                    std::array<std::byte, 60> input{}, output{};
                    unsigned width = format == Format::Uint16 ? 2 : 4;
                    for (unsigned i = 0; i < n; ++i) {
                        unsigned index = i * 7 + 2;
                        std::memcpy(input.data() + i * width, &index, width);
                    }
                    auto result = Convert(topology, format, std::span{input}.first(n * width),
                                          output, {}, provoking);
                    assert(std::memcmp(input.data(), output.data(), result.bytes) == 0);
                }
            }
        }
    }
    assert(NeedsConversion(Topology::Triangles, Format::Uint8, {}, Provoking::First));
    assert(NeedsConversion(Topology::Triangles, Format::Uint16, {true, 5}, Provoking::First));
    assert(NeedsConversion(Topology::Triangles, Format::Uint16, {}, Provoking::Last));
    assert(NeedsConversion(Topology::TriangleStrip, Format::Uint16, {}, Provoking::First));
    assert(NeedsConversion(Topology::Quads, Format::Uint32, {}, Provoking::First));
    assert(NeedsConversion(Topology::Points, static_cast<Format>(99), {}, Provoking::First));
    using Pso::AlphaTestSpecialization;
    assert(AlphaTestSpecialization(false, 2, 0) == AlphaTestSpecialization(false, 7, 0xdeadbeef));
    assert(AlphaTestSpecialization(true, 8, 123) == AlphaTestSpecialization(false, 1, 0));
    assert(AlphaTestSpecialization(true, 2, 123) != AlphaTestSpecialization(true, 2, 124));
    assert(AlphaTestSpecialization(true, 2, 123) != AlphaTestSpecialization(true, 3, 123));
    FrameProfile profile;
    DxcCache cache{&profile};
    auto first = cache.Compile("good", Shader::Stage::Fragment, 1);
    assert(cache.Compile("good", Shader::Stage::Fragment, 999) == first);
    assert(calls == 1);
    for (unsigned pipeline = 0; pipeline < 100; ++pipeline) {
        try { cache.Compile("bad", Shader::Stage::Fragment, pipeline); assert(false); }
        catch (const std::runtime_error& error) {
            assert(std::strcmp(error.what(), "synthetic DXC validation failure") == 0);
        }
    }
    assert(calls == 2); // Failed source is not compiled again for distinct PSO keys.
    try { cache.Compile("bad", Shader::Stage::VertexB); assert(false); }
    catch (const std::runtime_error&) {}
    assert(calls == 3); // Target profile still participates in the stable key.
    assert(cache.Compile("different", Shader::Stage::Fragment));
    assert(calls == 4);
    assert(profile.counts[static_cast<unsigned>(FrameCount::NegativeCacheHits)] == 99);
    assert(profile.counts[static_cast<unsigned>(FrameCount::DxcCompiles)] == 4);
}
"""
PROFILE_STUB = r"""
#pragma once
#include <array>
namespace D3D12 {
enum class FramePhase { DxcCompile };
enum class FrameCount { NegativeCacheHits, DxcCacheMisses, DxcCompiles };
inline bool DxcStrictEnabled() noexcept { return false; }
class FrameProfile {
public:
    struct Scope { Scope(FrameProfile&, FramePhase) {} };
    std::array<unsigned, 3> counts{};
    void Count(FrameCount count) { ++counts[static_cast<unsigned>(count)]; }
};
}
"""


class StallProfileTests(unittest.TestCase):
    def test_native_equivalence_specialization_and_failed_source_cache(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-stall-") as directory:
            temp = Path(directory)
            stub = temp / "video_core/renderer_d3d12/d3d12_context.h"
            stub.parent.mkdir(parents=True)
            stub.write_text(PROFILE_STUB)
            driver = temp / "driver.cpp"
            driver.write_text(DRIVER)
            binary = temp / "test"
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(temp),
                    "-I",
                    str(ROOT / "src"),
                    str(driver),
                    str(BACKEND / "d3d12_index_converter.cpp"),
                    str(BACKEND / "d3d12_dxc.cpp"),
                    str(BACKEND / "d3d12_disk_pipeline_cache.cpp"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=15)

    def test_every_profile_enum_has_a_log_name_and_submit_reasons_are_exhaustive(self):
        header = (BACKEND / "d3d12_context.h").read_text()
        source = (BACKEND / "d3d12_context.cpp").read_text()
        for enum, names in [("FramePhase", "names"), ("FrameCount", "counters")]:
            body = header.split(f"enum class {enum} : size_t {{")[1].split("};")[0]
            values = [v.strip() for v in body.split(",") if v.strip() and v.strip() != "Count"]
            logged = source.split(f"constexpr std::array {names}")[1].split("};")[0]
            self.assertEqual(len(values), len(re.findall(r'"[^"]+"', logged)))
        submit = source.split("u64 Context::Submit(")[1].split("void Context::Finish")[0]
        self.assertLess(submit.index("if (!recording)"), submit.index("profile.Count(reason)"))
        self.assertIn("profile.EndFrame(false)", submit)
        for filename, reason in [
            ("d3d12_descriptor.cpp", "ForcedSubmitDescriptorExhaustion"),
            ("d3d12_query_cache.cpp", "ForcedSubmitQueryBank"),
            ("d3d12_context.cpp", "ForcedSubmitMemoryPressure"),
            ("d3d12_context.cpp", "ForcedSubmitStagingPressure"),
        ]:
            self.assertIn(reason, (BACKEND / filename).read_text())
