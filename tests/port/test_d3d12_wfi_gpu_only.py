# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Guest wait-for-idle ran a CPU Finish ~34 times per frame in SM3DW. It must stay a GPU-side
barrier like the Vulkan backend, with the CPU wait kept only behind NXBOX_D3D12_WFI_FINISH."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class WaitForIdleGpuOnly(unittest.TestCase):
    def test_wait_for_idle_uses_gpu_barrier(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        body = re.search(r"void RasterizerD3D12::WaitForIdle\(\) \{(.*?)\n\}", source, re.S).group(1)
        self.assertIn("context.GlobalUavBarrier();", body)
        self.assertIn('BisectEnabled("NXBOX_D3D12_WFI_FINISH")', body)
        self.assertLess(body.index("if (cpu_wait)"), body.index("context.Finish("))

    def test_context_has_global_uav_barrier(self):
        source = (D3D12 / "d3d12_context.cpp").read_text()
        self.assertIn("D3D12_RESOURCE_BARRIER_TYPE_UAV", source)
        self.assertIn("barrier.UAV.pResource = nullptr;", source)


if __name__ == "__main__":
    unittest.main()
