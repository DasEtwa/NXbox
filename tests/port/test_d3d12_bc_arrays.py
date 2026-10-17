# SPDX-License-Identifier: GPL-3.0-or-later
"""Native BCn 2D arrays sample shifted layers on Xbox; they decode on the CPU instead."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class BcArrayDecodeTests(unittest.TestCase):
    def setUp(self):
        text = (D3D12 / "d3d12_texture_cache.cpp").read_text()
        start = text.index("native_bc_arrays =")
        self.rule = text[start : text.index("resource = MakeTexture", start)]

    def test_rule_covers_2d_bcn_arrays_but_not_cubes_or_single_layers(self):
        for needle in (
            'BisectEnabled("NXBOX_D3D12_NATIVE_BC_ARRAYS")',
            "IsPixelFormatBCn(info.format)",
            "info.type == VideoCommon::ImageType::e2D",
            "info.resources.layers > 1",
            "info.resources.layers % 6 != 0",
            "BcnFallbackFormat(host_format)",
        ):
            self.assertIn(needle, self.rule)

    def test_texel_cap_bounds_the_decoded_memory(self):
        self.assertIn("MaxDecodedArrayTexels = u64{4} * 1024 * 1024", self.rule)
        self.assertIn("<= MaxDecodedArrayTexels", self.rule)

    def test_bc4_decodes_to_r8_per_layer(self):
        formats = (D3D12 / "d3d12_formats.h").read_text()
        self.assertIn('starts_with("BC4")', formats)
        self.assertIn('"R8_UNORM"', formats)


if __name__ == "__main__":
    unittest.main()
