"""D3D12 samplers never get MinLOD above MaxLOD; with no mip filter both match Vulkan."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class SamplerMinLod(unittest.TestCase):
    def test_min_lod_is_clamped(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.cpp").read_text()
        block = source[source.index("D3D12_SAMPLER_DESC MakeSamplerDesc") :]
        block = block[: block.index("return desc;")]
        self.assertIn("desc.MinLOD = no_mips ? 0.0f : std::min(entry.MinLod(), desc.MaxLOD);", block)
        self.assertNotIn("desc.MinLOD = entry.MinLod();", block)

    def test_comparison_only_when_shader_compares(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.cpp").read_text()
        block = source[source.index("D3D12_SAMPLER_DESC MakeSamplerDesc") :]
        block = block[: block.index("return desc;")]
        self.assertIn("entry.depth_compare_enabled && shader_compares", block)
        self.assertNotIn("if (entry.depth_compare_enabled)", block)
        rasterizer = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        self.assertEqual(rasterizer.count("desc.is_depth);"), 2)


if __name__ == "__main__":
    unittest.main()
