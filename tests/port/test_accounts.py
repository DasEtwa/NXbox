# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable policy tests; these do not build or link the MSVC-only UWP application."""

import base64
import hashlib
import json
import re
import subprocess
import tempfile
import unittest
import zlib
from io import BytesIO
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    Image = None

ROOT = Path(__file__).resolve().parents[2]


class AccountsTests(unittest.TestCase):
    def test_first_launch_migration_and_avatar_path(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-accounts-") as temporary:
            binary = Path(temporary) / "accounts"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/accounts_policy.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_catalogue_counts(self):
        catalogue = json.loads((ROOT / "tools/nxbox-pack/src/avatar-catalog.json").read_text())
        self.assertGreaterEqual(len(catalogue["characters"]), 24)
        self.assertEqual(len(catalogue["colors"]), 12)
        self.assertEqual(len({c["name"] for c in catalogue["colors"]}), 12)

    @unittest.skipIf(Image is None, "Pillow is required for PNG pixel verification")
    def test_embedded_catalogue_and_transparent_masters(self):
        source = (ROOT / "src/core/constants_nxbox.h").read_text()
        arrays = re.findall(r"std::array<u8, (\d+)> (NXBOX_\w+) = \{(.*?)\};", source, re.S)
        catalogue = json.loads((ROOT / "tools/nxbox-pack/src/avatar-catalog.json").read_text())
        self.assertGreaterEqual(len(catalogue["characters"]), 24)
        self.assertEqual(len(catalogue["colors"]), 12)
        self.assertEqual(len({c["name"] for c in catalogue["colors"]}), 12)
        unique = set()
        for size, name, content in arrays:
            data = bytes(int(value, 16) for value in re.findall(r"0x([a-f0-9]{2})", content))
            self.assertEqual(len(data), int(size))
            image = Image.open(BytesIO(data))
            self.assertEqual(image.size, (256, 256))
            if name != "NXBOX_ACCOUNT_JPEG":
                self.assertEqual(image.format, "PNG")
                self.assertEqual(image.mode, "RGBA")
                self.assertEqual(image.getextrema()[3][0], 0)
            else:
                self.assertEqual(
                    hashlib.sha256(data).hexdigest(),
                    "60acf1f103b2482c1411d2e2146bef7b6310a62ba4905690655f2a45c6c4ebb1",
                )
            unique.add(data)
        self.assertEqual(len(unique), len(catalogue["characters"]) + 1)
        for character in catalogue["characters"]:
            image = Image.open(ROOT / "tools/nxbox/avatars" / (character["id"] + ".png"))
            self.assertEqual(image.size, (512, 512))
            self.assertEqual(image.mode, "RGBA")
            rgba = image.resize((256, 256), Image.Resampling.LANCZOS).tobytes()
            pixels = zlib.decompress(base64.b64decode(character["rgba"]))
            self.assertEqual(len(pixels), 256 * 256 * 4)
            # Compare real translucent-edge samples, not only a synthetic fixture.
            for i in range(0, len(rgba), 4):
                self.assertEqual(pixels[i + 3], rgba[i + 3])
                for channel in range(3):
                    self.assertEqual(
                        pixels[i + channel], (rgba[i + channel] * rgba[i + 3] + 127) // 255
                    )
        self.assertTrue((ROOT / "tools/nxbox/avatars/sheet.png").is_file())

    def test_single_mode_has_no_avatar_choices(self):
        source = (ROOT / "src/eden_uwp/ui/accounts_screen.cpp").read_text()
        self.assertIn("row_ == 2 && state_.multi", source)
        self.assertIn("if (!state_.multi || !UpdatePreview())", source)
        self.assertIn("if (state_.multi) {", source)
        policy = (ROOT / "src/eden_uwp/accounts_policy.h").read_text()
        self.assertIn("(multi && launching)", policy)
