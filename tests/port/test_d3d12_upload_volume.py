# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Upload cache behavior plus wiring contracts for Windows-only command recording."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class UploadVolumeTests(unittest.TestCase):
    def test_snapshot_lifetime_padding_invalidation_and_native_indices(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "upload-volume"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/d3d12_upload_volume_test.cpp"),
                    str(NATIVE / "d3d12_index_converter.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_recording_invalidates_even_without_a_transition_barrier(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        for begin, end in [
            ("void Context::Transition(", "void Context::TransitionBatch("),
            ("void Context::TransitionBatch(", "void Context::"),
        ]:
            section = source.split(begin, 1)[1].split(end, 1)[0]
            self.assertLess(
                section.index("++resource->content_version"),
                section.index("resource->state == state"),
            )
            for state in ("COPY_DEST", "UNORDERED_ACCESS", "STREAM_OUT"):
                self.assertIn("D3D12_RESOURCE_STATE_" + state, section)

    def test_identity_policy_uses_live_flat_inputs_and_keeps_resident_streams(self):
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        self.assertIn("PrepareDrawIndices(is_indexed, draw.params, *pipeline)", source)
        self.assertIn("fragment->loads.Generic(i)", source)
        self.assertIn("vertex->stores[Shader::IR::Attribute::Layer]", source)
        self.assertIn("vertex->stores[Shader::IR::Attribute::ViewportIndex]", source)
        self.assertIn("vertex_only && (has_flat_inputs || has_primitive_outputs)", source)
        self.assertIn("BindHostGeometryBuffers(is_indexed, !converted_indices.has_value())", source)
        direct = source.split("if (!realign && required <= available)", 1)[1].split("continue;", 1)[
            0
        ]
        self.assertIn("slot.resource->handle->GetGPUVirtualAddress() + source_offset", direct)
        self.assertNotIn("CopyBufferRegion", direct)
        allocator = (NATIVE / "d3d12_descriptor.cpp").read_text()
        self.assertIn("context.Submit(FrameCount::ForcedSubmitDescriptorExhaustion)", allocator)

    def test_dirty_upload_sources_are_reported_at_the_upload_not_the_binding(self):
        source = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        sync = source.split("bool BufferCache<P>::SynchronizeBuffer(", 1)[1].split(
            "\ntemplate <class P>", 1
        )[0]
        self.assertLess(
            sync.index("if (total_size_bytes == 0)"), sync.index("runtime.RecordBufferUpload")
        )
        self.assertIn("memory_tracker.ForEachUploadRange", sync)
        for category in ("Vertex", "Index", "Uniform", "Storage", "Texture"):
            self.assertIn("BufferUploadSource::" + category, source)
        self.assertIn("runtime.PreferCachedUniformBuffers()", source)
        runtime = (NATIVE / "d3d12_buffer_cache.cpp").read_text()
        self.assertNotIn("upload.Request(65536)", runtime)
        self.assertIn(
            "uniform_copies.Find(resource, resource->content_version, offset, size, &copy_offset)",
            runtime,
        )


if __name__ == "__main__":
    unittest.main()
