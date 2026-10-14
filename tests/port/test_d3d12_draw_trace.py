# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep the one-frame trace tied to submitted draws and final native PSO state."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class DrawTraceTests(unittest.TestCase):
    def test_frame_selection_cap_and_recording_boundary(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        trace = source.split("static const u64 draw_trace_frame", 1)[1].split(
            "        draw(commands);", 1
        )[0]
        self.assertIn('std::getenv("NXBOX_D3D12_DRAW_TRACE_FRAME")', trace)
        self.assertIn("std::from_chars", trace)
        self.assertIn("parsed.ptr != text.data() + text.size()", trace)
        self.assertIn("static const bool draw_trace_enabled = draw_trace_frame != 0", trace)
        self.assertIn("if (draw_trace_enabled)", trace)
        self.assertIn("texture_cache.CaptureFrame() == draw_trace_frame", trace)
        self.assertIn("draw_trace_lines < 3000", trace)
        self.assertEqual(trace.count("Diagnostic("), 1)
        self.assertIn("++draw_trace_lines", trace)
        cache = (ROOT / "src/video_core/texture_cache/texture_cache_base.h").read_text()
        self.assertRegex(cache, r"CaptureFrame\(\).*?\{\s*return frame_tick \+ 1;")
        self.assertIn("return pass_dump_has_set ? pass_dump_sequence + 1 : 0", cache)

    def test_submitted_state_and_root_constant_words(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        for field in (
            "frame",
            "draw",
            "pass",
            "rt0_gpu",
            "topology",
            "count",
            "instances",
            "is_indexed",
            "guest_indexed",
            "generated_index",
            "num_rts",
            "rtv",
            "has_depth",
            "dsv",
            "readonly_depth",
            "viewport",
            "scissor",
            "render_area",
            "nx_render_area",
            "nx_y_direction",
            "nx_resolution_down_factor",
            "vb0_addr_lo",
            "vb0_size",
            "vb0_stride",
            "ib_size",
            "ib_format",
            "ps_hash",
            "vs_hash",
        ):
            self.assertIn(f"{field}=", source)
        self.assertIn("quads && converted_indices.has_value()", source)
        self.assertIn("DXGI_FORMAT_R8G8B8A8_UNORM", source)
        self.assertIn("submitted.num_vertices", source)
        self.assertIn("submitted.num_instances, submitted.is_indexed", source)
        self.assertIn("std::bit_cast<float>(words[offset])", source)
        self.assertRegex(source, r"trace_float\(3\), trace_float\(5\),\s*trace_float\(4\)")
        self.assertIn("trace_vertex.BufferLocation & 0xffffffffULL", source)

    def test_final_descriptor_snapshot_covers_all_creation_paths(self):
        source = (D3D12 / "d3d12_graphics_pipeline.cpp").read_text()
        fields = (
            "num_rts",
            "rtv",
            "dsv",
            "samples",
            "sample_mask",
            "depth_enable",
            "depth_write",
            "depth_func",
            "stencil_enable",
            "rt0_blend",
            "rt0_src",
            "rt0_dst",
            "rt0_op",
            "rt0_write_mask",
            "independent_blend",
            "alpha_to_coverage",
            "cull",
            "front_ccw",
            "depth_clip",
            "fill",
            "topology_type",
        )
        self.assertEqual(set(re.findall(r"pso_(\w+)=", source)), set(fields))
        snapshot = source.index("draw_trace_pso = fmt::format")
        for creation in (
            "if (state.stencil_enable & 2)",
            "if (state.depth_bounds)",
            "disk->LoadOrCreateGraphics",
            "device->CreateGraphicsPipelineState",
        ):
            self.assertLess(snapshot, source.index(creation))
        self.assertIn("desc.SampleMask", source[snapshot:])
        self.assertIn("desc.PrimitiveTopologyType", source[snapshot:])

    def test_guest_hashes_survive_sync_and_async_compilation(self):
        source = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        self.assertRegex(source, r"disk.get\(\)\),\s*stage.hash}")
        self.assertRegex(source, r"request_hash, disk\),\s*stage.hash}")
        pipeline = (D3D12 / "d3d12_graphics_pipeline.cpp").read_text()
        self.assertIn("vertex_guest_hash = program.guest_hash", pipeline)
        self.assertIn("pixel_guest_hash = program.guest_hash", pipeline)
        self.assertIn("DumpGraphicsHlsl(guest_hashes[index], program.stage", source)


if __name__ == "__main__":
    unittest.main()
