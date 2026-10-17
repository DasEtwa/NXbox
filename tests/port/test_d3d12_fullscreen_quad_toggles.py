# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Diagnostic toggles for SM3DW fullscreen passes that never land on Xbox."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class FullscreenQuadToggles(unittest.TestCase):
    def test_toggles_are_wired(self):
        rasterizer = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        cache = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        for name in ("NXBOX_D3D12_QUAD_WHOLE_STREAM", "NXBOX_D3D12_VIEWPORT_Z01"):
            self.assertIn(f'BisectEnabled("{name}")', rasterizer)
        for name in ("NXBOX_D3D12_NO_CULL", "NXBOX_D3D12_NO_DEPTH_CLIP"):
            self.assertIn(f'BisectEnabled("{name}")', cache)
        self.assertIn("MinDepth={} > MaxDepth={}", rasterizer)


if __name__ == "__main__":
    unittest.main()
