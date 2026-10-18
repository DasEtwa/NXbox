# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Min/max samplers fall back to standard filtering where the device lacks tiled resources tier 2.

The Xbox Series reports tier 1 and removes the device on CreateSampler with a min/max filter
(Super Mario Bros. Wonder: DXGI_ERROR_INVALID_CALL). Found by JulianDr14/jadyx (GPL-3.0).
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class MinMaxSampler(unittest.TestCase):
    def test_context_reports_the_tier(self):
        source = (D3D12 / "d3d12_context.cpp").read_text()
        self.assertIn("options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2", source)
        self.assertIn("SetMinMaxFilteringSupported(minmax);", source)

    def test_sampler_falls_back_before_encoding(self):
        source = (D3D12 / "d3d12_descriptor.cpp").read_text()
        block = source[source.index("D3D12_SAMPLER_DESC MakeSamplerDesc") :]
        block = block[: block.index("return desc;")]
        guard = block.index("!minmax_filtering_supported.load()")
        self.assertLess(guard, block.index("desc.Filter ="))
        self.assertIn("reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;", block[guard:])


if __name__ == "__main__":
    unittest.main()
