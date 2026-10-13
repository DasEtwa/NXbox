"""BotW compute shaders with hundreds of `precise` values stalled the Xbox driver for minutes;
without the qualifier they compiled in under a second. Compute drops it, other stages keep it."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HLSL = ROOT / "src/shader_recompiler/backend/hlsl"


class HlslComputePrecise(unittest.TestCase):
    def test_compute_drops_precise(self):
        context = (HLSL / "hlsl_emit_context.cpp").read_text()
        self.assertIn("var_alloc.drop_precise = stage == Stage::Compute;", context)
        alloc = (HLSL / "var_alloc.cpp").read_text()
        self.assertIn('drop_precise || NoPrecise() ? "float" : "precise float"', alloc)
        self.assertIn('drop_precise || NoPrecise() ? "double" : "precise double"', alloc)

    def test_blocklist_waits_a_minute(self):
        cache = (ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp").read_text()
        self.assertIn("IsStalled(key, std::chrono::seconds{60})", cache)
        self.assertIn("_d3d12_stalled_pso_v2.txt", cache)


if __name__ == "__main__":
    unittest.main()
