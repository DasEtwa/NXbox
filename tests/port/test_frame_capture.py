# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate frame comparison metrics and frontend capture wiring."""

import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class FrameCaptureTests(unittest.TestCase):
    def test_frame_diff_metrics_and_heatmaps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            a, b, out = root / "a", root / "b", root / "report"
            a.mkdir()
            b.mkdir()
            header = b"P6\n2 1\n255\n"
            pixels = bytes([0, 0, 0, 40, 80, 120])
            for directory in (a, b):
                (directory / "frame_000001.ppm").write_bytes(header + pixels)
            (a / "frame_000002.ppm").write_bytes(header + pixels)
            (b / "frame_000002.ppm").write_bytes(header + bytes([255, 0, 0, 40, 80, 120]))
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/nxbox/frame_diff.py"),
                    str(a),
                    str(b),
                    "--out",
                    str(out),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("frame_000001.ppm: PSNR=inf dB, pixels >16=0.0000%", result.stdout)
            expected_psnr = 10 * math.log10(6)
            self.assertIn(
                f"frame_000002.ppm: PSNR={expected_psnr:.4f} dB, pixels >16=50.0000%",
                result.stdout,
            )
            self.assertEqual((out / "report.txt").read_text(), result.stdout)
            self.assertEqual(
                (out / "diff_frame_000001.ppm").read_bytes(),
                header + bytes([0, 0, 0, 10, 20, 30]),
            )
            self.assertEqual(
                (out / "diff_frame_000002.ppm").read_bytes(),
                header + bytes([255, 0, 0, 10, 20, 30]),
            )

    def test_frame_diff_matches_drifted_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            a, b, out = root / "a", root / "b", root / "report"
            a.mkdir()
            b.mkdir()
            header = b"P6\n1 1\n255\n"
            (a / "frame_000010.ppm").write_bytes(header + bytes([0, 0, 0]))
            (a / "frame_000012.ppm").write_bytes(header + bytes([200, 10, 10]))
            (a / "frame_000040.ppm").write_bytes(header + bytes([200, 10, 10]))
            (b / "frame_000011.ppm").write_bytes(header + bytes([200, 10, 10]))
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/nxbox/frame_diff.py"),
                    str(a),
                    str(b),
                    "--out",
                    str(out),
                    "--match-window",
                    "5",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                result.stdout,
                "frame_000011.ppm: PSNR=inf dB, pixels >16=0.0000% (A frame_000012.ppm)\n",
            )

    def test_renderer_and_cli_capture_sources(self):
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        self.assertNotIn("screenshot capture is stubbed", renderer)
        cli = (ROOT / "src/yuzu_cmd/yuzu.cpp").read_text()
        helper = (ROOT / "src/yuzu_cmd/frame_capture.h").read_text()
        self.assertIn('#include "yuzu_cmd/frame_capture.h"', cli)
        self.assertIn('std::getenv("NXBOX_CAPTURE_FRAMES")', cli)
        self.assertIn("renderer.RequestScreenshot(", helper)
        self.assertIn("RequestFrame(state->system.Renderer(), 1)", cli)


if __name__ == "__main__":
    unittest.main()
