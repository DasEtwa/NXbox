"""A sampled texture that aliases the bound render target synchronizes from it in FillImageViews.
If the target was marked modified only before that, both end with the same modification tick
and the draw's output never reaches the texture (SM3DW bloom mips stayed black)."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class RenderTargetsAfterTextures(unittest.TestCase):
    def test_graphics_marks_targets_after_filling_views(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        fill = re.search(r"texture_cache\.FillImageViews\([^;]*false,\s*has_images\);", source)
        self.assertIsNotNone(fill)
        tail = source[fill.end() : fill.end() + 800]
        self.assertIn("texture_cache.UpdateRenderTargets(false);", tail)


if __name__ == "__main__":
    unittest.main()
