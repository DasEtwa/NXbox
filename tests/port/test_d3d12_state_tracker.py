# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit production register dependencies and execute the allocation-free cache input logic."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class StateTrackerTests(unittest.TestCase):
    def test_input_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "state-cache"
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
                    str(ROOT / "tests/port/d3d12_state_cache_test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_register_coverage(self):
        pipeline = (NATIVE / "d3d12_pipeline_cache.cpp").read_text()
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        tracker = (NATIVE / "d3d12_state_tracker.cpp").read_text()
        functions = [
            function(pipeline, "void FillGraphicsState("),
            function(pipeline, "const char* UnsupportedState("),
            function(pipeline, "const auto compute_request ="),
            function(rasterizer, "ViewportResult MakeViewport("),
            function(rasterizer, "ViewportResult MakeFullViewport("),
            function(rasterizer, "D3D12_RECT MakeScissor("),
            function(
                rasterizer, "RasterizerD3D12::DynamicState RasterizerD3D12::ComputeDynamicState("
            ),
        ]
        names = set(re.findall(r"regs\.(\w+)", "\n".join(functions)))
        names.remove("IsShaderConfigEnabled")
        # Follow the indirect helper, rather than treating it as a register.
        maxwell = (ROOT / "src/video_core/engines/maxwell_3d.h").read_text()
        shader_config = function(maxwell, "bool IsShaderConfigEnabled(std::size_t")
        self.assertIn("pipelines[index].enable", shader_config)
        names.add("pipelines")
        tracked = set(re.findall(r"TRACK\((\w+)", tracker))
        for name in sorted(names - tracked):
            if name in ("zeta_enable", "pipelines"):
                # Both slots can be owned by shared caches. Require actual production input
                # comparisons at each consumer, preserving every shared dependency.
                self.assertNotIn(f"TRACK({name});", tracker)
                if name == "zeta_enable":
                    pattern = r"regs.zeta_enable,\s*is_indexed"
                else:
                    pattern = r"regs.pipelines\[2\].enable != 0, regs.pipelines\[3\].enable != 0"
                self.assertEqual(len(re.findall(pattern, pipeline)), 1)
                self.assertEqual(len(re.findall(pattern, rasterizer)), 2)
            elif name == "vertex_streams":
                self.assertIn("MAXWELL3D_REG_INDEX(vertex_streams)", tracker)
                self.assertIn("offsetof(Maxwell3D::Regs::VertexStream, frequency)", tracker)
                self.assertNotIn("TRACK(vertex_streams)", tracker)
            else:
                self.fail(f"Untracked register dependency: {name}")
        self.assertIn("TRACK(transform_feedback.controls)", tracker)
        self.assertNotIn("TRACK(transform_feedback);", tracker)
        # Reject a new aliased stream member that would escape frequency-only registration.
        fill = functions[0]
        self.assertEqual(re.findall(r"regs.vertex_streams\[[^]]+\]\.(\w+)", fill), ["frequency"])

    def test_invalidation_and_async_pending(self):
        context = (NATIVE / "d3d12_context.h").read_text()
        # Register-derived caches survive a new recording (queries and labels invalidate it
        # almost every draw); RecordState re-emits the command-list state on its own.
        self.assertNotIn(
            "graphics_state_tracker.Invalidate();",
            function(context, "void InvalidateRecordedState()"),
        )
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        self.assertIn(
            "StateTracker::SetupDirtyFlags(dirty.tables)",
            function(rasterizer, "void RasterizerD3D12::InitializeChannel"),
        )
        bind = function(rasterizer, "void RasterizerD3D12::BindChannel")
        self.assertIn("StateTracker::Bind(channel.payload->maxwell_3d)", bind)
        self.assertIn("context.GraphicsStateTracker().Invalidate()", bind)
        tracker = (NATIVE / "d3d12_state_tracker.cpp").read_text()
        self.assertIn("RescaleViewports", tracker)
        self.assertIn("RescaleScissors", tracker)
        pipeline = (NATIVE / "d3d12_pipeline_cache.cpp").read_text()
        current = function(pipeline, "IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline")
        self.assertIn("if (last_graphics_pipeline && request == last_graphics_request)", current)
        self.assertIn("auto entry = async->Lookup(request)", current)
        self.assertIn("if (!reuse || StateVerifyEnabled())", current)
        self.assertIn("StateFastpathEnabled() && graphics_state_inputs.Matches(inputs)", current)

    def test_hash_compatibility_and_bounded_diagnostics(self):
        header = (NATIVE / "d3d12_pipeline_cache.h").read_text()
        self.assertIn("Common::CityHash64", function(header, "struct GraphicsPipelineKeyHash"))
        context = (NATIVE / "d3d12_context.cpp").read_text()
        self.assertIn('GetEnvironmentVariableA("NXBOX_D3D12_STATE_FASTPATH"', context)
        self.assertIn("value[0] == '0'", function(context, "bool StateFastpathEnabled()"))
        self.assertIn('BisectEnabled("NXBOX_D3D12_STATE_VERIFY")', context)
        mismatch = function(context, "void StateVerifyMismatch")
        self.assertIn("std::array<Mismatch, 32>", mismatch)
        self.assertIn("count == reported.size()", mismatch)
        self.assertIn("D3D12_STATE_VERIFY mismatch what={}", mismatch)
        self.assertNotIn("mutex", mismatch)
        self.assertNotIn("unordered", mismatch)


if __name__ == "__main__":
    unittest.main()
