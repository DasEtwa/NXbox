"""Draws reuse fixed RTV/DSV slots and rewrite them only when the attachments change.
Recreating them every draw cost ~5 descriptor writes per draw (18k per BotW frame)."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class BoundTargets(unittest.TestCase):
    def test_draw_path_uses_fixed_slots_behind_a_change_check(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        self.assertIn("descriptors.BoundRtvs()", source)
        check = source.index("if (targets != bound_targets ||")
        block = source[check : check + 1200]
        self.assertIn("descriptors.CreateRtv(", block)
        self.assertIn("descriptors.CreateDsv(", block)
        # Every CreateRtv/CreateDsv in the draw path sits inside the change check.
        draw = source[source.index("// Render targets: rewrite") : check]
        self.assertNotRegex(draw, r"descriptors\.Create(Rtv|Dsv)\(")

    def test_key_holds_resources_and_dsv_flags(self):
        header = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.h").read_text()
        key = re.search(r"struct BoundTargets \{.*?\n    \};", header, re.S).group(0)
        for field in ("ResourcePtr", "dsv_flags", "layers", "num_rts", "format"):
            self.assertIn(field, key)


if __name__ == "__main__":
    unittest.main()
