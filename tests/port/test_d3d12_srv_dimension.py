# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""SRVs take the dimension the shader declares, not the guest view type.
A mismatch is undefined in D3D12: vkd3d tolerates it, the Xbox driver samples zeros."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class SrvDimension(unittest.TestCase):
    def setUp(self):
        self.source = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.cpp").read_text()

    def test_write_image_srv_adapts_to_the_shader_dimension(self):
        body = self.source[self.source.index("bool D3D12DescriptorAllocator::WriteImageSrv") :]
        body = body[: body.index("\n}\n")]
        self.assertIn("AdaptSrvDimension(desc, fallback_dimension", body)
        self.assertLess(body.index("AdaptSrvDimension"), body.index("WriteCached("))

    def test_every_array_and_plain_pair_converts(self):
        adapt = self.source[self.source.index("AdaptSrvDimension(\n") :]
        adapt = adapt[: adapt.index("\n}\n")]
        for case in ("TEXTURE1D:", "TEXTURE1DARRAY:", "TEXTURE2D:", "TEXTURE2DARRAY:",
                     "TEXTURECUBE:", "TEXTURECUBEARRAY:"):
            self.assertRegex(adapt, r"case D3D12_SRV_DIMENSION_" + re.escape(case))


if __name__ == "__main__":
    unittest.main()
