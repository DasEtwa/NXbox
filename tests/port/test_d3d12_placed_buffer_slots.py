# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

import pathlib
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[2] / "src/video_core/renderer_d3d12/d3d12_context.cpp"


class PlacedBufferSlots(unittest.TestCase):
    def test_slot_count_rounds_up_and_never_zero(self):
        text = SOURCE.read_text()
        self.assertNotIn("allocation.SizeInBytes / BufferHeapSlots::Alignment)", text)
        self.assertIn("(allocation.SizeInBytes + BufferHeapSlots::Alignment - 1) /", text)
        self.assertIn("std::max<u64>(1,", text)

    def test_new_page_allocation_failure_is_not_dereferenced(self):
        text = SOURCE.read_text()
        tail = text[text.index("pages.push_back(page);"):]
        self.assertLess(tail.index("if (!start)"), tail.index("*start"))


if __name__ == "__main__":
    unittest.main()
