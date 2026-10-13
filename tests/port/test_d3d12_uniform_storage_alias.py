"""SM3DW binds a constant buffer and a storage buffer inside the same host buffer. D3D12 cannot
hold one buffer in CONSTANT_BUFFER and UNORDERED_ACCESS at once, so those draws were skipped
("conflicting resource states") and the title-screen UI text vanished. Constants that alias a
storage binding must be read from a copy, in graphics draws and compute dispatches alike."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class UniformStorageAlias(unittest.TestCase):
    def test_both_cbv_paths_copy_aliased_uniforms(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        guard = "if (buffer.resource && IsStorageBuffer(bindings, buffer.resource))"
        self.assertEqual(source.count(guard), 2)
        self.assertEqual(source.count("buffer = buffer_runtime.CopyUniform(buffer);"), 2)

    def test_misaligned_uniforms_share_the_copy(self):
        source = (D3D12 / "d3d12_buffer_cache.cpp").read_text()
        self.assertIn("BoundBuffer BufferCacheRuntime::CopyUniform(const BoundBuffer& uniform)", source)
        self.assertIn("CopyUniform({std::move(resource), offset, size})", source)


if __name__ == "__main__":
    unittest.main()
