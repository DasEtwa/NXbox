# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Point sprite coordinates and undeclared-descriptor errors in the HLSL emitter.

The tester log of a failed async pipeline only carried std::vector::at's "invalid
vector subscript" or "HLSL attribute PointSpriteT is not implemented". The harness
cases behind this test pin the replacements: a 1px point reads the sprite centre, and
a descriptor index outside its declaration list says which list, index and stage.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class HlslPointSpriteDescriptorTests(unittest.TestCase):
    def test_point_sprite_and_undeclared_descriptors(self):
        root = Path(__file__).resolve().parents[2]
        cmake = shutil.which("cmake")
        binary = os.environ.get("NXBOX_HLSL_IR_TEST_BINARY")
        if not binary and not cmake:
            self.skipTest("CMake required to build HLSL IR fixtures")
        with tempfile.TemporaryDirectory(prefix="nxbox-hlsl-pointsprite-") as directory:
            if not binary:
                build = Path(directory) / "build"
                subprocess.run(
                    [cmake, "-S", str(root / "tests/port"), "-B", str(build)],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=180,
                )
                # The real IR harness requires fmt and Boost, but no renderer or GPU.
                if not (build / "CMakeFiles/hlsl-ir-tests.dir").exists():
                    self.skipTest("fmt/Boost unavailable for the HLSL IR harness")
                subprocess.run(
                    [cmake, "--build", str(build), "--target", "hlsl-ir-tests", "--parallel", "2"],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=600,
                )
                binary = str(build / "hlsl-ir-tests")
            result = subprocess.run(
                [binary, "point-sprite-descriptors"],
                cwd=directory,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            source = Path(directory) / "hlsl_pointsprite.hlsl"
            self.assertTrue(source.exists(), "point sprite case did not emit HLSL")
            text = source.read_text()
            self.assertIn("output.color0.x = ", text)
            self.assertIn("0.5f", text)
            self.assertNotIn("PointSprite", text)


if __name__ == "__main__":
    unittest.main()
