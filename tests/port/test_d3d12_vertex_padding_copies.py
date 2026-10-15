"""Padded vertex streams copy the data and the zero tail into disjoint ranges.

D3D12 does not order two copies into the same range without a barrier; on Xbox the
zero fill landed after the data and SM3DW's fullscreen quads collapsed to a point.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VertexPaddingCopies(unittest.TestCase):
    def test_zero_fill_only_covers_the_tail(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        block = source[source.index('context.MarkWork("vertex binding padding");') :]
        block = block[: block.index("context.Retain(zero);")]
        self.assertIn("copied);", block)
        self.assertIn("if (required > copied)", block)
        self.assertRegex(
            block,
            r"CopyBufferRegion\(padded->handle.Get\(\), copied,\s*"
            r"zero->handle.Get\(\), shared_zero \? 0 : copied,\s*required - copied\);",
        )
        self.assertLess(block.index("slot.resource->handle.Get()"), block.rindex("zero->handle.Get()"))


if __name__ == "__main__":
    unittest.main()
