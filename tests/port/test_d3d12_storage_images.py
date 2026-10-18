# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Typeless and signed storage images are checked against the bound view before binding, and a
bind that cannot work skips its draw or dispatch through SkipDraw with the reason."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class StorageImages(unittest.TestCase):
    def setUp(self):
        self.source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()

    def test_every_problem_skips_the_draw(self):
        calls = [m.start() for m in re.finditer(r"StorageImageProblem\(context", self.source)]
        self.assertEqual(len(calls), 4)
        for call in calls:
            self.assertIn("SkipDraw(problem)", self.source[call : call + 1000])

    def test_numeric_kind_and_cached_support(self):
        helper = self.source[self.source.index("const char* StorageImageProblem(") :]
        helper = helper[: helper.index("\n}\n")]
        self.assertIn("IsPixelFormatInteger(format) != is_integer", helper)
        self.assertIn("FormatSupport(", helper)
        self.assertNotIn("CheckFeatureSupport", helper)


if __name__ == "__main__":
    unittest.main()
