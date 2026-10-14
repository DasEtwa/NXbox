"""BC transfer footprints use whole blocks. The D3D12 runtime rejects a 2x2/1x1 BC
footprint (vkd3d accepts it), so the small mips of BC textures were never uploaded on
Xbox; SM3DW samples its irradiance cube at those mips and levels went black."""

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
