# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Reinterpreting between equal-size color formats waited on the GPU for every copy (~10 per
frame in BotW) because it went through CPU staging. Without host conversion the bytes match, so
the copy bounces through a GPU buffer, and the CPU path stays only for converted formats."""

import re
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2] / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp"


class ReinterpretOnGpu(unittest.TestCase):
    def test_gpu_path_runs_before_cpu_staging(self):
        source = SOURCE.read_text()
        body = re.search(r"void TextureCacheRuntime::ReinterpretImage\(.*?\n\}", source, re.S).group(0)
        self.assertLess(body.index("ReinterpretOnGpu(dst, src, copies)"), body.index("DownloadStagingBuffer"))

    def test_gpu_path_never_waits_and_rejects_conversions(self):
        source = SOURCE.read_text()
        body = re.search(r"bool TextureCacheRuntime::ReinterpretOnGpu\(.*?\n\}", source, re.S).group(0)
        self.assertNotIn("Finish", body)
        self.assertIn("from.conversion != FormatConversion::None", body)
        self.assertIn("from.depth || to.depth", body)
        self.assertIn("D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT", body)


if __name__ == "__main__":
    unittest.main()
