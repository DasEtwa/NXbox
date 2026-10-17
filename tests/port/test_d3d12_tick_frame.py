# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""RasterizerD3D12::TickFrame was never called, so the texture cache never ran garbage
collection and the buffer, fence and descriptor caches never aged a frame. The compositor
must tick it once per composed frame, as RendererVulkan::Composite does."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class D3D12TickFrame(unittest.TestCase):
    def test_composite_ticks_rasterizer(self):
        source = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        body = re.search(r"void RendererD3D12::Composite\(.*?\n\}", source, re.S).group(0)
        self.assertEqual(body.count("rasterizer.TickFrame();"), 1)
        self.assertLess(body.index("gpu.RendererFrameEndNotify();"), body.index("rasterizer.TickFrame();"))


if __name__ == "__main__":
    unittest.main()
