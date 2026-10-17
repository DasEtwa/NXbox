# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""NXBOX_D3D12_SHADOW_ONE makes HLSL depth-compare samples and gathers return 1."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ShadowOneToggle(unittest.TestCase):
    def test_toggle_is_wired(self):
        image = (ROOT / "src/shader_recompiler/backend/hlsl/emit_hlsl_image.cpp").read_text()
        cache = (ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp").read_text()
        self.assertIn("ctx.profile.hlsl_shadow_one && depth_compare", image)
        self.assertIn("EmitCubeOne(ctx, inst, info, false, !dref.empty())", image)
        self.assertIn('BisectEnabled("NXBOX_D3D12_SHADOW_ONE")', cache)


if __name__ == "__main__":
    unittest.main()
