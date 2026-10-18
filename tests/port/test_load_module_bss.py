# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Module loading skips the image's trailing zeros (.bss): the code region is already zero.

On the Xbox the guest backing is committed on first touch, so writing a large .bss full of
zeros charged the 5 GiB app budget at load for memory the game may never use."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class LoadModuleBss(unittest.TestCase):
    def test_trailing_zeros_are_not_written(self):
        source = (ROOT / "src/core/hle/kernel/k_process.cpp").read_text()
        body = source.split("void KProcess::LoadModule(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("std::find_if(image.rbegin(), image.rend()", body)
        self.assertIn("WriteBlock(base_addr, image.data(), written)", body)
        self.assertNotIn("code_set.memory.size())", body)

    def test_new_code_pages_are_still_zero(self):
        # Skipping the zero tail is only correct while freshly mapped pages are cleared to zero.
        table = (ROOT / "src/core/hle/kernel/k_page_table_base.cpp").read_text()
        self.assertIn("const bool fill_memory = false;", table)
        impl = table.split("Result KPageTableBase::AllocateAndMapPagesImpl(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("ClearBackingRegion(m_system, it.GetAddress(), it.GetSize(), m_heap_fill_value);", impl)


if __name__ == "__main__":
    unittest.main()
