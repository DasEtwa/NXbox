# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production descriptor-set cache without the Windows SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class DescriptorTableTests(unittest.TestCase):
    def test_table_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "descriptor-tables"
            build = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/d3d12_descriptor_tables_test.cpp"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            subprocess.run([str(binary)], check=True)

    def test_windows_lifetime_and_copy_wiring(self):
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        header = (NATIVE / "d3d12_descriptor.h").read_text()
        self.assertIn('BisectEnabled("NXBOX_D3D12_DESCRIPTOR_TABLE_CACHE")', source)
        finish = source.split("DescriptorTable D3D12DescriptorAllocator::FinishDrawTable", 1)[1]
        finish = finish.split("void D3D12DescriptorAllocator::WriteCached", 1)[0]
        self.assertLess(finish.index("return cached->table;"), finish.index("AllocateSamplers"))
        self.assertEqual(finish.count("RememberDrawTable(plan, table, sampler);"), 2)
        self.assertIn("{table, plan.resources}", source)
        self.assertIn("draw_table_cache[sampler ? 1 : 0].Find(context.CurrentTick()", source)
        frame = header.split("void TickFrame()", 1)[1].split("private:", 1)[0]
        self.assertIn("cache.Clear()", frame)


if __name__ == "__main__":
    unittest.main()
