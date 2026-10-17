# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Parts of the UWP build have no RTTI: dynamic_cast/typeid there throw
std::__non_rtti_object ("Access violation - no RTTI data!"). Twice it silently
broke the D3D12 renderer, so keep these directories free of them."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIRS = ["src/video_core/renderer_d3d12", "src/eden_uwp"]
FILES = ["src/video_core/gpu_thread.cpp", "src/video_core/video_core.cpp"]
PATTERN = re.compile(r"\b(dynamic_cast\s*<|typeid\s*\()")


class NoRttiCasts(unittest.TestCase):
    def test_no_dynamic_cast_or_typeid(self):
        paths = [p for d in DIRS for p in (ROOT / d).rglob("*") if p.suffix in (".cpp", ".h")]
        paths += [ROOT / f for f in FILES]
        hits = []
        for path in paths:
            for number, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
                code = line.split("//", 1)[0]
                if PATTERN.search(code):
                    hits.append(f"{path.relative_to(ROOT)}:{number}")
        self.assertEqual(hits, [])


if __name__ == "__main__":
    unittest.main()
