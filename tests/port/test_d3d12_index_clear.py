# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute std-only native index conversion and clear selection under sanitizers."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class D3D12IndexClearTests(unittest.TestCase):
    def compile_and_run(self, module):
        with tempfile.TemporaryDirectory(prefix=f"nxbox-{module}-") as temp:
            binary = Path(temp) / module
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-g",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / f"tests/port/{module}_test.cpp"),
                    str(ROOT / f"src/video_core/renderer_d3d12/{module}.cpp"),
                    "-o",
                    str(binary),
                ],
                check=False,
                timeout=60,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_exhaustive_index_conversion(self):
        self.compile_and_run("d3d12_index_converter")

    def test_clear_masks_and_scissor(self):
        self.compile_and_run("d3d12_clear")
