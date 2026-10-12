"""Compile and execute the std-only HLSL binding and root ABI tests."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class HlslHelperTests(unittest.TestCase):
    def test_host_helpers(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if compiler is None:
            self.skipTest("A host C++ compiler is required")
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="nxbox-hlsl-") as directory:
            binary = Path(directory) / "hlsl-helper-tests"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{root / 'src'}",
                    str(root / "tests/port/hlsl_helpers.cpp"),
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
