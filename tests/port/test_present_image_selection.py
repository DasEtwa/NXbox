# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production display lookup with conflicting live image aliases."""

import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run
from test_texture_cache_fastpath import function

ROOT = Path(__file__).resolve().parents[2]


class PresentImageSelectionTests(unittest.TestCase):
    def test_display_geometry_format_recency_and_view_creation(self):
        source = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        lookup = function(source, "TextureCache<P>::TryFindFramebufferImageView(")
        lookup += "\n" + function(source, "TextureCache<P>::PrepareImageForPresentation(")
        harness = (ROOT / "tests/port/fixtures/present_image_selection.cpp").read_text()
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "fmt"], text=True))
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                harness.replace("// PRODUCTION_METHOD", lookup),
                Path(directory),
                extra_flags=[*flags, "-fsanitize=address,undefined", "-Wall", "-Wextra"],
            )

    def test_presenter_fallback_downloads_gpu_owned_alias_before_guest_upload(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        signature = "RasterizerD3D12::DisplayTexture RasterizerD3D12::FramebufferTexture("
        body = source.split(signature, 1)[1].split("\n}\n", 1)[0]
        presenter = signature + body + "\n}\n"
        harness = (ROOT / "tests/port/fixtures/present_framebuffer.cpp").read_text()
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                harness.replace("// PRODUCTION_METHOD", presenter),
                Path(directory),
                extra_flags=["-fsanitize=address,undefined", "-Wall", "-Wextra"],
            )


if __name__ == "__main__":
    unittest.main()
