# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sanitize the scanner, exact cache hash recompute, and bounded trace runtime."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ShaderDiscoveryTests(unittest.TestCase):
    def test_scanner_and_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            (temporary / "common").mkdir()
            (temporary / "common/logging.h").write_text(
                "#pragma once\n#define LOG_INFO(category, ...) TestLog(__VA_ARGS__)\n"
            )
            (temporary / "video_core").mkdir()
            (temporary / "video_core/memory_manager.h").write_text(
                "#pragma once\n#include <cstdint>\n#include <cstring>\n#include <vector>\n"
                "namespace Tegra { class MemoryManager { public:\n"
                "std::vector<std::uint8_t> bytes; std::uint64_t base{0x10000};\n"
                "std::size_t reads{}, max_read{};\n"
                "std::size_t GetID() const { return 1; }\n"
                "bool IsWithinGPUAddressRange(std::uint64_t at) const { return at < (1ULL << 40); }\n"
                "void ReadBlockUnsafe(std::uint64_t at, void* out, std::size_t size) {\n"
                "++reads; max_read = std::max(max_read, size); std::memset(out, 0, size);\n"
                "if (at >= base && at-base < bytes.size())\n"
                "std::memcpy(out, bytes.data()+at-base, std::min<std::size_t>(size, bytes.size()-(at-base)));\n"
                "} }; }\n"
            )
            binary = temporary / "shader-discovery"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    "-I",
                    str(temporary),
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/shader_discovery_test.cpp"),
                    str(ROOT / "src/common/cityhash.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                text=True,
            )
            env = dict(os.environ)
            env.pop("NXBOX_SHADER_DISCOVERY_TRACE", None)
            subprocess.run([str(binary)], check=True, env=env)
            subprocess.run([str(binary), "disabled"], check=True, env=env)

    def test_demand_hooks_precede_cache_returns(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp").read_text()
        graphics = source.split("IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(")[1]
        self.assertLess(
            graphics.index("ShaderDiscovery::Demand("),
            graphics.index("graphics_state_inputs.Matches("),
        )
        compute = source.split("ComputePipeline* PipelineCache::CurrentComputePipeline()")[1]
        self.assertLess(
            compute.index("ShaderDiscovery::Demand("), compute.index("guest_compute.find(")
        )
        scanner = (ROOT / "src/video_core/shader_discovery.cpp").read_text()
        self.assertIn("memory->ReadBlockUnsafe", scanner)
        self.assertNotIn("scoped_lock", scanner)
        self.assertNotIn("make_unique", scanner.split("void EndFrame(")[1])
        self.assertNotIn("GetPointer", scanner)
        upload = (ROOT / "src/video_core/engines/engine_upload.cpp").read_text()
        scalar = upload.split("void State::ProcessData(const u32 data,")[1].split(
            "void State::ProcessData(const u32* data,"
        )[0]
        self.assertIn("ShaderDiscovery::OwnedUpload", scalar)
        self.assertIn(
            "ShaderDiscovery::OwnedUpload",
            upload.split("void State::ProcessData(const u32* data,")[1],
        )


if __name__ == "__main__":
    unittest.main()
