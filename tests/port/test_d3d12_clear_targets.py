# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Guest clears build their own RTV/DSV. A view the resource cannot back is left unwritten by a
release runtime, and the Xbox driver reads a null pointer in ClearRenderTargetView (Super Mario
Odyssey crashed this way on Series S), so every clear view is checked before it is created."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ClearTargets(unittest.TestCase):
    def setUp(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        start = source.index("const auto prepare = [&](ImageView* view, bool is_depth")
        self.prepare = source[start : source.index("clear_renderer.Execute(", start)]

    def test_volume_targets_get_a_3d_rtv(self):
        self.assertIn("D3D12_RTV_DIMENSION_TEXTURE3D", self.prepare)
        self.assertIn("desc.Texture3D = {mip, layer, 1};", self.prepare)

    def test_invalid_views_are_skipped_before_creation(self):
        check = self.prepare.index("if (invalid) {")
        self.assertLess(check, self.prepare.index("CreateRenderTargetView("))
        self.assertLess(check, self.prepare.index("CreateDepthStencilView("))
        for reason in ("1D texture", "out of range", "cannot be a render target",
                       "cannot be a depth target"):
            self.assertIn(reason, self.prepare)
        self.assertIn("D3D12_FORMAT_SUPPORT1_RENDER_TARGET", self.prepare)
        self.assertIn("D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL", self.prepare)


if __name__ == "__main__":
    unittest.main()
