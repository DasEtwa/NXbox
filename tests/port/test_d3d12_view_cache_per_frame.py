"""BotW submits ~30 times per frame. Clearing the CPU view descriptor cache on every submit
recreated ~11k descriptors per frame, so the cache is cleared once per frame instead."""

import re
import unittest
from pathlib import Path

D3D12 = Path(__file__).resolve().parents[2] / "src/video_core/renderer_d3d12"


class ViewCachePerFrame(unittest.TestCase):
    def test_refresh_keeps_views_until_frame_ends(self):
        source = (D3D12 / "d3d12_descriptor.cpp").read_text()
        body = re.search(r"void D3D12DescriptorAllocator::RefreshCaches\(\) \{(.*?)\n\}", source, re.S).group(1)
        self.assertLess(body.index("if (new_frame)"), body.index("view_cache.entries.clear()"))

    def test_rasterizer_ticks_descriptor_frame(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        body = re.search(r"void RasterizerD3D12::TickFrame\(\) \{(.*?)\n\}", source, re.S).group(1)
        self.assertIn("descriptors.TickFrame();", body)


if __name__ == "__main__":
    unittest.main()
