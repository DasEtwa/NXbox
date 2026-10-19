# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay production source-handle logic with sanitizers and check lifetime wiring."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class DescriptorFastpathTests(unittest.TestCase):
    def test_random_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "descriptor-fastpath"
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
                    str(ROOT / "tests/port/d3d12_descriptor_fastpath_test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_windows_wiring(self):
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        self.assertIn('std::getenv("NXBOX_D3D12_DESCRIPTOR_FASTPATH")', source)
        self.assertIn('std::strcmp(value, "0") != 0', source)
        self.assertIn("++view_cache.generation", source)
        self.assertIn("++cache.generation", source)
        self.assertIn("previous.plan.sources.Matches(plan.sources, cache.generation)", source)
        self.assertIn(
            "CopyDescriptors(1, &table.cpu, &count, count, sources.data(), nullptr,", source
        )
        writers = source.split("void D3D12DescriptorAllocator::WriteCached", 2)[2]
        writers = writers.split("std::optional<DescriptorTable>", 1)[0]
        self.assertNotIn("FramePhase::DescriptorWrites", writers)
        self.assertIn("RefreshCaches(); // Reserve may have submitted", source)


if __name__ == "__main__":
    unittest.main()
