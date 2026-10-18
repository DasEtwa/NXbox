# SPDX-FileCopyrightText: Copyright 2026 NSPX
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
        recording = source.split("        if (draw_trace_enabled)", 1)[1].split(
            "        draw(commands);", 1
        )[0]
        self.assertEqual(recording.count("Diagnostic("), 2)
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

    def test_vertex_bytes_are_opt_in_bounded_and_use_actual_ia_resources(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        selection = source.split("const bool trace_vertex_bytes =", 1)[1].split(
            "// Keep the actual", 1
        )[0]
        for condition in (
            "draw_trace_enabled",
            "texture_cache.CaptureFrame() == draw_trace_frame",
            "draw_trace_lines < 3000",
            "vb_trace_draws < 64",
            "!submitted.is_indexed",
            "submitted.num_vertices <= 64",
        ):
            self.assertIn(condition, selection)
        self.assertIn("constexpr size_t VertexTraceBytes = 48", source)
        self.assertIn("constexpr u32 MaxTraceStreams = 2", source)
        self.assertIn("trace_streams{0, 0}", source)
        self.assertIn("trace_attribute.enabled == 1 && trace_attribute.slot != 0", source)
        for resource in ("zero_vertex_buffer", "slot.resource", "padded", "packed.buffer"):
            self.assertRegex(
                source,
                rf"if \(trace_vertex_bytes\)\s+trace_vertex_resources\[i\] = {re.escape(resource)};",
            )
        allocation = source.index("context.CreateBuffer(VertexTraceBytes * MaxTraceStreams")
        reservation = source.index("const u64 table_tick = context.CurrentTick()")
        self.assertLess(allocation, reservation)
        trace = source.split("++vb_trace_draws;", 1)[1].split("        draw(commands);", 1)[0]
        self.assertNotIn("CreateBuffer", trace)
        self.assertIn("trace_guest_stream.Address() + trace_attribute_offset", trace)
        self.assertIn("trace_guest_stream.stride.Value()", trace)
        self.assertIn("submitted.base_vertex", trace)
        self.assertIn("submitted.base_instance", trace)
        self.assertIn("gpu_memory->ReadBlockUnsafe(trace_guest_address", trace)
        self.assertIn('fmt::format("{:02x}", trace_byte)', trace)
        self.assertIn("trace_resource->heap", trace)
        for kind in ('"cache"', '"zero"', '"padded"'):
            self.assertIn(kind, trace)

    def test_vertex_readback_is_ordered_retained_and_fence_completed(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        trace = source.split("++vb_trace_draws;", 1)[1].split("        draw(commands);", 1)[0]
        trace = re.sub(r"\s+", " ", trace)
        pause = trace.index("query_runtime.PauseHostConditionalRendering()")
        transition = trace.index(
            "context.Transition(trace_resource, D3D12_RESOURCE_STATE_COPY_SOURCE)"
        )
        copy = trace.index("commands->CopyBufferRegion(")
        restore = trace.index("context.Transition(trace_resource, trace_previous_state)")
        completion = trace.index("context.AfterCompletion(")
        resume = trace.index("query_runtime.ResumeHostConditionalRendering()")
        self.assertLess(pause, transition)
        self.assertLess(transition, copy)
        self.assertLess(copy, restore)
        self.assertLess(restore, completion)
        self.assertLess(completion, resume)
        self.assertIn("const auto trace_previous_state = trace_resource->state", trace)
        self.assertIn("trace_host_view.BufferLocation -", trace)
        self.assertIn("trace_resource->handle->GetGPUVirtualAddress()", trace)
        self.assertIn("trace_host_view.SizeInBytes - trace_view_first", trace)
        self.assertIn(
            "context.Transition(vertex_trace_readback, D3D12_RESOURCE_STATE_COPY_DEST)", trace
        )
        callback = trace[completion:resume]
        self.assertIn("trace_readback = vertex_trace_readback", callback)
        self.assertIn("trace_draw = draw_trace_lines", callback)
        self.assertIn("trace_readback->mapped.subspan(", callback)
        self.assertIn("D3D12_VB_TRACE draw={} slot={} guest={} host={} heap={}", callback)
        for forbidden in ("this", "gpu_memory", "context.", "Commands()", "Wait(", "Finish("):
            self.assertNotIn(forbidden, callback.removeprefix("context.AfterCompletion("))

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
            "depth_bias",
            "slope_bias",
            "bias_clamp",
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
