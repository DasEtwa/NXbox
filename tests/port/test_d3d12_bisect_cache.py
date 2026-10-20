# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""BisectEnabled caches each toggle per thread; a name past the cache reads the environment every call."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class BisectCacheTests(unittest.TestCase):
    def test_every_toggle_name_fits_the_cache(self):
        context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        size = int(re.search(r"std::array<Cached, (\d+)> cache", context).group(1))
        names = set()
        for path in (ROOT / "src").rglob("*.[ch]*"):
            names.update(
                re.findall(r'BisectEnabled\("([A-Z0-9_]+)"\)', path.read_text(errors="ignore"))
            )
        # Leave room for the toggles the next jobs add.
        self.assertLessEqual(len(names) + 16, size, f"{len(names)} names for {size} slots")


if __name__ == "__main__":
    unittest.main()
