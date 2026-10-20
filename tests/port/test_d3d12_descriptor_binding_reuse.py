# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare cached GPU descriptor/root state to unconditional binding on random draws."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class DescriptorBindingReuseTests(unittest.TestCase):
    def test_random_old_path_equivalence(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-descriptor-binding-") as directory:
            binary = Path(directory) / "test"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/d3d12_descriptor_binding_reuse_test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_root_cache_tracks_actual_writes_and_list_reset(self):
        commands = (NATIVE / "d3d12_commands.h").read_text()
        for name in (
            "32BitConstants",
            "ShaderResourceView",
            "UnorderedAccessView",
            "DescriptorTable",
        ):
            method = commands.split(f"void SetGraphicsRoot{name}(", 1)[1].split("\n    }", 1)[0]
            self.assertIn("graphics_root_cbvs.Invalidate(index);", method)
        self.assertIn("graphics_root_cbvs.SetSignature(", commands)
        self.assertIn("graphics_root_cbvs.cbvs.Update(index, address);", commands)
        context = (NATIVE / "d3d12_context.cpp").read_text()
        start = context.split("CommandFacade* Context::Commands()", 1)[1].split(
            "void Context::MarkWork", 1
        )[0]
        self.assertIn("facade.graphics_root_cbvs.Reset();", start)
        draw = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        self.assertIn('BisectEnabled("NXBOX_D3D12_ROOT_CBV_REUSE")', commands)
        self.assertIn("root_cbv_reuse ? root_changed : changed", draw)
        allocator = (NATIVE / "d3d12_descriptor.cpp").read_text()
        self.assertIn('std::getenv("NXBOX_D3D12_HANDLE_TABLE_CACHE")', allocator)
        self.assertIn("plan.sources.Current(cache.generation)", allocator)
        self.assertIn("FindHandleDrawTable(plan, sampler)", allocator)


if __name__ == "__main__":
    unittest.main()
