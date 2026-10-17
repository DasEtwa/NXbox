# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep exclusive draw phases wired to their diagnostic names and cache steps."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"
DRAW_PHASES = {
    "DrawDispatch": "draw_dispatch",
    "DrawMemory": "draw_memory",
    "GraphicsDescriptors": "graphics_descriptors",
    "RenderTargets": "render_targets",
    "AttachmentSetup": "attachment_setup",
    "QueryState": "query_state",
    "GraphicsState": "graphics_state",
    "ResourceBindings": "resource_bindings",
    "ImageViews": "image_views",
    "GraphicsBuffers": "graphics_buffers",
    "GeometryBuffers": "geometry_buffers",
    "StageBuffers": "stage_buffers",
    "BufferValidation": "buffer_validation",
    "FeedbackSetup": "feedback_setup",
    "GeometrySetup": "geometry_setup",
    "VertexCopies": "vertex_copies",
    "DescriptorSetup": "descriptor_setup",
    "DrawRecording": "draw_recording",
    "DrawCleanup": "draw_cleanup",
}


class DrawPhaseTests(unittest.TestCase):
    def test_enum_formatter_and_recording_scopes_agree(self):
        header = (BACKEND / "d3d12_context.h").read_text()
        source = (BACKEND / "d3d12_context.cpp").read_text()
        rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        enum_body = header.split("enum class FramePhase : size_t {")[1].split("};")[0]
        phases = [value.strip() for value in enum_body.split(",") if value.strip()]
        names_body = source.split("constexpr std::array names{")[1].split("};")[0]
        names = re.findall(r'"([^"]+)"', names_body)
        self.assertEqual(phases[-1], "Count")
        self.assertEqual(len(names), len(phases) - 1)
        for phase, name in DRAW_PHASES.items():
            with self.subTest(phase=phase):
                self.assertIn(phase, phases)
                self.assertEqual(names[phases.index(phase)], name)
                self.assertIn(f"FramePhase::{phase}", rasterizer)
        self.assertIn("BetweenDraws", phases)
        self.assertEqual(names[phases.index("BetweenDraws")], "between_draws")
        self.assertIn("FramePhase active{FramePhase::BetweenDraws}", header)
        self.assertIn("D3D12_FRAME_PROFILE thread=", source)
        self.assertIn('fmt::format(" {}_cpu_ms={:.3f}", names[i]', source)

    def test_shared_cache_calls_have_separate_top_level_scopes(self):
        source = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        prepare = source.split("void RasterizerD3D12::PrepareDraw(bool")[1].split(
            "bool RasterizerD3D12::PrepareDrawIndices"
        )[0]
        sections = re.split(
            r"draw_phase\.emplace\(context\.Profile\(\), FramePhase::(\w+)\);",
            prepare,
        )
        calls = {}
        for index in range(1, len(sections), 2):
            calls.setdefault(sections[index], "")
            calls[sections[index]] += sections[index + 1]
        for phase, call in {
            "DrawMemory": "gpu_memory->FlushCaching()",
            "GraphicsDescriptors": "texture_cache.SynchronizeDescriptors(false)",
            "RenderTargets": "texture_cache.UpdateRenderTargets(false)",
            "ImageViews": "texture_cache.FillImageViews(",
            "GraphicsBuffers": "buffer_cache.UpdateGraphicsBuffers(is_indexed)",
            "GeometryBuffers": "buffer_cache.BindHostGeometryBuffers(",
            "StageBuffers": "buffer_cache.BindHostStageBuffers(stage)",
            "ResourceBindings": "buffer_cache.BindGraphicsTextureBuffer(",
            "GeometrySetup": "Pso::RepackVertexFormat(",
            "DescriptorSetup": "descriptors.FinishDrawTable(",
            "DrawRecording": "commands->SetPipelineState(pso)",
            "QueryState": "query_runtime.BeginDraw(",
            "DrawCleanup": "stage.Clear()",
        }.items():
            with self.subTest(phase=phase):
                self.assertIn(call, calls[phase])
        self.assertEqual(calls["RenderTargets"].count("UpdateRenderTargets(false)"), 2)
        self.assertIn("std::optional<FrameProfile::Scope> draw_phase", prepare)
        self.assertIn("FramePhase::VertexCopies", calls["GeometrySetup"])
        self.assertIn("vertex binding padding", calls["GeometrySetup"])
        indices = source.split("bool RasterizerD3D12::PrepareDrawIndices(")[1].split(
            "void RasterizerD3D12::Draw(bool"
        )[0]
        self.assertEqual(indices.count("FramePhase::IndexConversion"), 1)
        self.assertLess(
            indices.index("FramePhase::IndexConversion"), indices.index("NeedsConversion(")
        )
        self.assertIn("FramePhase::BufferDownload", indices)
