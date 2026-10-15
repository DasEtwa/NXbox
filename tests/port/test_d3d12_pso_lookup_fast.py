"""D3D12 repeated draws reuse finished PSOs without querying device capabilities again."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp"
HEADER = ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.h"


class PsoLookupFast(unittest.TestCase):
    def test_feature_queries_are_cached_outside_lookup(self):
        source = SOURCE.read_text()
        header = HEADER.read_text()
        body = source.split("IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(", 1)[1]
        body = body.split("ComputePipeline* PipelineCache::CurrentComputePipeline()", 1)[0]
        self.assertNotIn("CheckFeatureSupport", body)
        self.assertIn("if (!cached.queried)", header)
        self.assertIn("cached.queried = true;", header)
        for suffix in ("", "2", "13", "14"):
            self.assertIn(f"CachedFeature<D3D12_FEATURE_DATA_D3D12_OPTIONS{suffix}>", header)
            self.assertIn(
                f"QueryFeature(D3D12_FEATURE_D3D12_OPTIONS{suffix}, graphics_options{suffix})", body
            )
        self.assertIn("format_support[format]", source)
        self.assertIn("FAILED(support.result)", body)

    def test_last_request_precedes_guest_map_lookup(self):
        source = SOURCE.read_text()
        body = source.split("IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(", 1)[1]
        body = body.split("ComputePipeline* PipelineCache::CurrentComputePipeline()", 1)[0]
        key_built = body.index("request.stage_hashes[0] =")
        fast_path = body.index("if (last_graphics_pipeline && request == last_graphics_request)")
        map_lookup = body.index("guest_graphics.find(request)")
        self.assertLess(key_built, fast_path)
        self.assertLess(fast_path, map_lookup)
        self.assertIn("return last_graphics_pipeline;", body)
        self.assertIn("RememberGraphicsPipeline(request, found->second)", body)
        self.assertIn("RememberGraphicsPipeline(request, entry.result->graphics.get())", body)
        self.assertIn("RememberGraphicsPipeline(request, &pipeline)", body)
        remember = source.split("IGraphicsPipeline* PipelineCache::RememberGraphicsPipeline", 1)[1]
        remember = remember.split("IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline", 1)[0]
        self.assertIn("if (pipeline)", remember)
        self.assertIn("last_graphics_pipeline = pipeline;", remember)

    def test_guest_mutations_and_lifecycle_invalidate(self):
        source = SOURCE.read_text()
        store = source.split("void PipelineCache::StoreGuestGraphics", 1)[1]
        store = store.split("IGraphicsPipeline* PipelineCache::RememberGraphicsPipeline", 1)[0]
        self.assertLess(
            store.index("InvalidateLastGraphicsPipeline();"), store.index("guest_graphics.emplace(")
        )
        self.assertEqual(source.count("guest_graphics.emplace("), 1)
        load = source.split("void PipelineCache::LoadDiskResources", 1)[1]
        self.assertLess(
            load.index("InvalidateLastGraphicsPipeline();"), load.index("async.reset();")
        )
        self.assertLess(
            load.index("InvalidateLastGraphicsPipeline();"), load.index("guest_graphics.clear();")
        )
        bind = source.split("void PipelineCache::BindToChannel", 1)[1].split(
            "bool PipelineCache::CheckStalled", 1
        )[0]
        self.assertIn("InvalidateLastGraphicsPipeline();", bind)


if __name__ == "__main__":
    unittest.main()
