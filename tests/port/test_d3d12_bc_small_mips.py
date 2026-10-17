# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""BC transfer footprints use whole blocks, the layout D3D12 documents for block
formats, instead of a 2x2/1x1 extent for the small mips."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class BcSmallMips(unittest.TestCase):
    def test_footprint_desc_is_block_aligned(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        start = source.index("// Use the SDK's layout for each transfer extent")
        block = source[start : source.index("GetCopyableFootprints(", start)]
        self.assertIn("AlignUp(u64{extent.width}, host_format.block_width)", block)
        self.assertIn("AlignUp(extent.height, host_format.block_height)", block)


if __name__ == "__main__":
    unittest.main()
