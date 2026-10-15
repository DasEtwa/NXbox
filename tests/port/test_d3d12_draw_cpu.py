"""Guard the dependency coverage and invalidation of per-draw state reuse."""

import re
import unittest
from pathlib import Path

BACKEND = Path(__file__).resolve().parents[2] / "src/video_core/renderer_d3d12"


class DrawCpu(unittest.TestCase):
    def test_every_graphics_key_register_has_dirty_tracking(self):
        source = (BACKEND / "d3d12_pipeline_cache.cpp").read_text()
        fill = source.split("void FillGraphicsState(", 1)[1].split(
            "ID3D12RootSignature* PipelineCache::GetRootSignature", 1
        )[0]
        lookup = source.split("IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(", 1)[1]
        specialization = lookup.split("const auto& regs = maxwell3d->regs;", 1)[1].split(
            "request.stage_hashes[0] =", 1
        )[0]
        inputs = set(re.findall(r"regs\.([a-z_]+)", fill + specialization))
        tracked = set(re.findall(r"TRACK\(([a-z_]+)", source))
        # Vertex frequency is the only key input in vertex_streams; map each stream.
        self.assertIn("offsetof(Maxwell::VertexStream, frequency)", source)
        tracked.add("vertex_streams")
        self.assertFalse(inputs - tracked, f"Untracked pipeline inputs: {inputs - tracked}")
        self.assertIn("graphics_stage_hashes == guest_hashes", lookup)
        self.assertIn("graphics_generation == context.RecordingGeneration()", lookup)
        self.assertIn("graphics_topology == current_topology", lookup)
        self.assertIn("!collision_changed", lookup)
        self.assertIn("graphics_formats.colors == formats.colors", lookup)
        self.assertIn("graphics_formats.depth == formats.depth", lookup)
        self.assertIn("graphics_formats.sample_count == formats.sample_count", lookup)
        # Shared cache bits can be consumed before lookup; compare colliding registers instead.
        self.assertIn("graphics_collision_indices.push_back", source)
        self.assertIn("if (a == VideoCommon::Dirty::NullEntry)", source)
        self.assertIn("else if (b == VideoCommon::Dirty::NullEntry)", source)

    def test_all_predication_writers_share_the_recording_cache(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        setter = context.split("void Context::SetDrawPredication", 1)[1].split(
            "void Context::MarkWork", 1
        )[0]
        self.assertLess(setter.index("Commands()"), setter.index("DrawState()"))
        self.assertIn("predicate.Update(predicate)", setter)
        for path in BACKEND.glob("*.cpp"):
            if path.name != "d3d12_context.cpp":
                self.assertNotIn("->SetPredication(", path.read_text(), path.name)
        query = (BACKEND / "d3d12_query_cache.cpp").read_text()
        self.assertIn("impl->context.SetDrawPredication(nullptr)", query)
        self.assertIn("impl->context.SetDrawPredication(impl->predicate->handle.Get())", query)
        # Query accumulation is never skipped just because draw state did not change.
        self.assertIn("commands->BeginQuery(", query)
        self.assertIn("commands->EndQuery(", query)
        self.assertIn("commands->ResolveQueryData(", query)
