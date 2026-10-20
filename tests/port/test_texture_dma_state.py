# SPDX-License-Identifier: GPL-3.0-or-later
"""Differential checks of production texture state and DMA packet fast paths."""

import unittest
from pathlib import Path

import test_gpu_frontend_hot_paths as frontend

method = frontend.method

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/port/fixtures"


class TextureDmaStateTests(unittest.TestCase):
    compile_and_run = frontend.GpuFrontendHotPathTests.compile_and_run

    def test_dma_against_frozen_prechange_parser(self):
        source = (ROOT / "src/video_core/dma_pusher.cpp").read_text()
        methods = "\n".join(
            method(source, f"DmaPusher::{name}")
            for name in ("CallMethod", "CallMultiMethod", "ProcessCommands")
        )
        fixture = (FIXTURES / "gpu_frontend_dma.cpp").read_text()
        # Both engines use the same batching. Only the parser differs.
        fixture = fixture.replace("DmaPusher scalar;", "ReferencePusher scalar;")
        fixture = fixture.replace(
            "scalar.frontend_fastpath = false;", "scalar.frontend_fastpath = true;"
        )
        fixture = fixture.replace(
            "assert(scalar.engine.increasing_batches == 0 &&\n"
            "               scalar.engine1.increasing_batches == 0); // hatch off => no batching",
            "// Singleton writes can bypass the bulk handler; compare effects below.",
        )
        fixture = fixture.replace("1 + random() % 6", "random() % 7")
        fixture = fixture.replace(
            "SubmissionMode::Inline,     SubmissionMode::IncreaseOnce",
            "SubmissionMode::Inline, SubmissionMode::IncreaseOnce, "
            "static_cast<SubmissionMode>(0), static_cast<SubmissionMode>(2), "
            "static_cast<SubmissionMode>(6), static_cast<SubmissionMode>(7)",
        )
        fixture = fixture.replace(
            "if (mode != SubmissionMode::Inline)",
            "if (mode == SubmissionMode::Increasing || mode == SubmissionMode::NonIncreasing || "
            "mode == SubmissionMode::IncreaseOnce)",
        )
        self.compile_and_run(
            fixture.replace(
                "// METHODS", methods + "\n" + (FIXTURES / "dma_pusher_legacy.inc").read_text()
            )
        )

    def test_texture_dirty_state_randomized(self):
        source = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        sync = method(source, "TextureCache<P>::SynchronizeDescriptors")
        sync = sync.replace("TextureCache<P>", "Cache")
        update = method(source, "TextureCache<P>::UpdateRenderTargets")
        # Compile the complete clean branch, ending before the unchanged dirty path.
        update = update[
            : update.index("    const VideoCommon::RenderTargets previous_render_targets")
        ]
        update += "    ++dirty_updates;\n}"
        update = update.replace("TextureCache<P>", "Cache")
        fill = method(source, "TextureCache<P>::FillImageViews")
        fill = fill[: fill.index("    if (PassDump::GetConfig().enabled")] + "}"
        fill = fill.replace("TextureCache<P>", "Cache")
        self.compile_and_run(
            (FIXTURES / "texture_draw_state.cpp")
            .read_text()
            .replace("// METHODS", sync + "\n" + update + "\n" + fill)
        )


if __name__ == "__main__":
    unittest.main()
