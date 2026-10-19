# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and execute the emitter's source reduction unit tests."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class HlslSourceTests(unittest.TestCase):
    def test_source_reduction(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if compiler is None:
            self.skipTest("A host C++ compiler is required")
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="nxbox-hlsl-source-") as directory:
            binary = Path(directory) / "hlsl-source-tests"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{root / 'src'}",
                    str(root / "tests/port/hlsl_source.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
