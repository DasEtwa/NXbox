# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Incomplete files are listed after every playable game, so the first tile and focus can play."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class IncompleteLast(unittest.TestCase):
    def test_incomplete_entries_sort_after_playable_ones(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        self.assertIn("return LibraryContent::Less(left, right,", library)
        helper = (ROOT / "src/frontend_common/library_content.h").read_text()
        sort = helper[helper.index("bool Less(") :]
        rule = "if (left.incomplete != right.incomplete)\n        return right.incomplete;"
        self.assertIn(rule, sort)
        self.assertLess(sort.index(rule), sort.index("compare_name(left.name"))


if __name__ == "__main__":
    unittest.main()
