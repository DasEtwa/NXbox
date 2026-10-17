# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise dump paths and retained artifacts without UWP or a graphics driver."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ShaderDumpTests(unittest.TestCase):
    def test_paths_replacements_and_retained_artifacts(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "dump.cpp"
            executable = Path(directory) / "dump"
            source.write_text(r"""
#include <cassert>
#include <thread>
#include "common/nxbox_shader_dump.h"
using namespace NxboxShaderProbe;
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path local{argv[1]};
    const auto root = local / "shader_dump";
    const auto replace = local / "shader_replace";
    const Hash hash = 0xabcdef;
    assert(PipelineFolder(root, hash) == root / "0000000000abcdef");
    assert(PipelineFolder(root, ~Hash{}) == root / "ffffffffffffffff");
    assert(StageFile(replace, hash, 0, ".glsl") ==
           replace / "0000000000abcdef" / "vertex.glsl");
    assert(StageFile(replace, hash, 4, ".glsl").filename() == "fs.glsl");
    assert(GuestStages[0] == "va" && GuestStages[1] == "vb");
    assert(HostStages[1] == "tcs" && HostStages[2] == "tes" && HostStages[3] == "gs");
    assert(!ReadReplacement({}, hash, 0));
    assert(!ReadReplacement(replace, hash, 0));
    std::filesystem::create_directories(PipelineFolder(replace, hash));
    const std::string glsl = "#version 450\nvoid main() {}\n";
    { std::ofstream file(StageFile(replace, hash, 0, ".glsl"), std::ios::binary); file << glsl; }
    assert(ReadReplacement(replace, hash, 0) == glsl);
    assert(!ReadReplacement(replace, hash + 1, 0));
    assert(!ReadReplacement(replace, hash, 4));
    { std::ofstream file(StageFile(replace, hash, 4, ".glsl")); }
    bool failed = false;
    try { ReadReplacement(replace, hash, 4); } catch (const std::runtime_error&) { failed = true; }
    assert(failed);
    DumpArtifacts artifacts;
    // Capture while a worker owns its own input; destroy the original before selection.
    { const std::string guest{"\0\x01\x02\x03", 4};
      artifacts.Add("vb.maxwell.bin", guest.data(), guest.size()); }
    artifacts.Add("vertex.glsl", glsl.data(), glsl.size());
    const std::array<unsigned char, 52> key{};
    artifacts.Add("pipeline.key.bin", key.data(), key.size());
    artifacts.stages = 1;
    Config config;
    assert(!config.Dump(hash, {}));
    assert(config.dump.Parse(Hex(hash)) && config.Dump(hash, {}));
    auto write = [&](Hash selected) {
        const std::lock_guard lock(artifacts.mutex);
        assert(artifacts.Write(root, selected) == 4 + glsl.size() + key.size());
    };
    std::filesystem::create_directories(PipelineFolder(root, hash));
    { std::ofstream file(StageFile(root, hash, 0, ".spv")); file << "obsolete"; }
    { std::ofstream file(PipelineFolder(root, hash) / "notes.txt"); file << "keep"; }
    std::thread first(write, hash), second(write, hash + 1);
    first.join(); second.join();
    assert(ReadReplacement(root, hash, 0) == glsl);
    assert(!std::filesystem::exists(StageFile(root, hash, 0, ".spv")));
    assert(std::filesystem::exists(PipelineFolder(root, hash) / "notes.txt"));
    assert(std::filesystem::file_size(PipelineFolder(root, hash) / "pipeline.key.bin") == 52);
    std::ifstream binary(PipelineFolder(root, hash) / "vb.maxwell.bin", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{binary}, {}};
    assert(bytes == std::string("\0\x01\x02\x03", 4));
    // A failed write does not masquerade as a successful key-only dump.
    const auto blocked = local / "blocked";
    { std::ofstream file(blocked); file << "not a directory"; }
    failed = false;
    try { artifacts.Write(blocked, hash); } catch (const std::filesystem::filesystem_error&) { failed = true; }
    assert(failed && !artifacts.written);
    assert(artifacts.Write(root, hash) == 4 + glsl.size() + key.size());
}
""")
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
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
            subprocess.run([str(executable), directory], check=True)

    def test_cache_hit_and_mixed_backend_hooks(self):
        cache = (ROOT / "src/video_core/renderer_opengl/gl_shader_cache.cpp").read_text()
        hit = cache.split("GraphicsPipeline* ShaderCache::BuiltPipeline", 1)[1].split(
            "ComputePipeline* ShaderCache::CurrentComputePipeline", 1
        )[0]
        self.assertLess(hit.index("DumpPipeline"), hit.index("IsBuilt"))
        self.assertIn("pipeline->dump_artifacts", hit)
        query = cache.split("GraphicsPipeline* ShaderCache::CurrentGraphicsPipeline()", 1)[1].split(
            "GraphicsPipeline* ShaderCache::CurrentGraphicsPipelineSlowPath", 1
        )[0]
        self.assertIn("dump_selection != last_dump_selection", query)
        self.assertIn("for (const auto& [key, pipeline] : graphics_cache)", query)
        self.assertLess(query.index("DumpPipeline"), query.index("RefreshStages"))
        pipeline = (ROOT / "src/video_core/renderer_opengl/gl_graphics_pipeline.cpp").read_text()
        spirv = pipeline.split("case Settings::RendererBackend::OpenGL_SPIRV:", 1)[1].split(
            "default:", 1
        )[0]
        self.assertLess(spirv.index("sources_[stage]"), spirv.index("sources_spirv_[stage]"))
        self.assertIn("ReadReplacement(replacement_folder, hash, i)", cache)
        self.assertIn("sources_spirv[i].clear()", cache)
        self.assertIn("static_vector<Shader::Environment*, Maxwell::MaxShaderProgram>", cache)
        self.assertIn("pipeline->dump_artifacts = std::move(artifacts)", cache)

    def test_late_selection_and_concurrent_dump(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "winrt").mkdir()
            (folder / "eden_uwp").mkdir()
            (folder / "winrt/Windows.Storage.h").write_text(r"""
#pragma once
#include <string>
namespace winrt::Windows::Storage {
inline std::string local;
struct Folder { std::string Path() const { return local; } };
struct ApplicationData {
    static ApplicationData Current() { return {}; }
    Folder LocalFolder() const { return {}; }
};
}
""")
            (folder / "eden_uwp/diagnostic.h").write_text(r"""
#pragma once
#include <mutex>
#include <string>
#include <vector>
namespace EdenXbox {
inline std::mutex mutex;
inline std::vector<std::string> lines;
inline void Diagnostic(const std::string& line) {
    const std::lock_guard lock(mutex);
    lines.push_back(line);
}
}
""")
            source = folder / "late.cpp"
            executable = folder / "late"
            source.write_text(r"""
#include <cassert>
#include <thread>
#define NXBOX_UWP
#include "common/nxbox_shader_probe_uwp.h"
using namespace NxboxShaderProbe;
int main(int argc, char** argv) {
    assert(argc == 2);
    winrt::Windows::Storage::local = argv[1];
    unsetenv("NXBOX_DUMP_SHADERS");
    assert(Settings().dump.ranges.empty()); // lifetime config has already been read
    const Hash hash = 0x123;
    DumpArtifacts artifacts;
    artifacts.Add("pipeline.key.bin", "key", 3);
    artifacts.Add("vb.maxwell.bin", "guest", 5);
    artifacts.Add("vertex.spv", "spirv", 5);
    artifacts.stages = 1;
    DumpPipeline(artifacts, hash, {});
    assert(!artifacts.written && EdenXbox::lines.empty());
    setenv("NXBOX_DUMP_SHADERS", "123,456,789", 1);
    // Simulate a disk reconstruction worker racing a render-thread cache query.
    std::thread worker([&] { DumpPipeline(artifacts, hash, {}); });
    DumpPipeline(artifacts, hash, {});
    worker.join();
    assert(artifacts.written && EdenXbox::lines.size() == 1);
    assert(EdenXbox::lines[0] == "SHADER_DUMP hash=0000000000000123 stages=1 bytes=13");
    for (Hash other : {0x456, 0x789}) {
        DumpArtifacts cached;
        cached.Add("fs.glsl", "fragment", 8);
        cached.stages = 1;
        DumpPipeline(cached, other, {});
        assert(cached.written);
    }
    setenv("NXBOX_DUMP_SHADERS", "not-a-hash", 1);
    assert(!DumpSelected(hash, {}));
    setenv("NXBOX_DUMP_SHADERS", "abc", 1);
    assert(DumpSelected(hash, {0, 0, 0, 0, 0, 0xabc}));
    unsetenv("NXBOX_SHADER_REPLACE_DIR");
    assert(ReplacementFolder().empty());
    setenv("NXBOX_SHADER_REPLACE_DIR", "1", 1);
    assert(ReplacementFolder() == std::filesystem::path(argv[1]) / "shader_replace");
    setenv("NXBOX_SHADER_REPLACE_DIR", "shader_replace", 1);
    assert(ReplacementFolder() == std::filesystem::path(argv[1]) / "shader_replace");
    setenv("NXBOX_SHADER_REPLACE_DIR", argv[1], 1);
    assert(ReplacementFolder() == std::filesystem::path(argv[1]));
    setenv("NXBOX_SHADER_REPLACE_DIR", "0", 1);
    assert(ReplacementFolder().empty());
}
""")
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-I",
                    directory,
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
            subprocess.run([str(executable), directory], check=True)
