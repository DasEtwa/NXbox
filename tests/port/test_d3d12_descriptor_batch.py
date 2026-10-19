# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production batch helper and check Windows-only cache lifetime wiring."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class DescriptorBatchTests(unittest.TestCase):
    def test_counting_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "descriptor-batch"
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
                    str(ROOT / "tests/port/d3d12_descriptor_batch_test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_rollover_flushes_before_source_reuse(self):
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        rollover = source.split("if (cache.cursor == cache.capacity) {", 1)[1]
        self.assertLess(
            rollover.index("FlushCopies(type, *batch)"), rollover.index("cache.cursor = 0")
        )
        finish = source.split("DescriptorTable D3D12DescriptorAllocator::FinishDrawTable", 1)[1]
        finish = finish.split("void D3D12DescriptorAllocator::WriteCached", 1)[0]
        self.assertIn(
            'static const bool batch_copies = BisectEnabled("NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES")',
            finish,
        )
        self.assertLess(finish.index("return previous.table"), finish.index("batch.emplace()"))
        self.assertLess(
            finish.rindex("FlushCopies(type, *batch)"), finish.rindex("previous.tick =")
        )
        self.assertIn("catch (...) {", finish)
        self.assertIn("table.Slot(slot), batch ? &*batch : nullptr", finish)

    def test_counter_and_api_contract(self):
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        self.assertIn("destinations, nullptr,", source)
        self.assertIn("sources, nullptr, type)", source)
        self.assertEqual(source.count("Count(FrameCount::DescriptorCopyCalls)"), 3)
        self.assertIn('"descriptor_copy_calls"', (NATIVE / "d3d12_context.cpp").read_text())
        self.assertIn("DescriptorCopyCalls,", (NATIVE / "d3d12_context.h").read_text())


if __name__ == "__main__":
    unittest.main()
