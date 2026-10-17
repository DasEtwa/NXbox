# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""NXBOX_D3D12_CBV_FULL widens every CBV to 64 KiB, still clamped to the allocation."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CbvFullToggle(unittest.TestCase):
    def test_toggle_precedes_allocation_clamp(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.cpp").read_text()
        block = source[source.index("void D3D12DescriptorAllocator::WriteCbv") :]
        block = block[: block.index("WriteCached(")]
        toggle = block.index('BisectEnabled("NXBOX_D3D12_CBV_FULL")')
        self.assertLess(toggle, block.index("buffer.offset + size > width"))


if __name__ == "__main__":
    unittest.main()
