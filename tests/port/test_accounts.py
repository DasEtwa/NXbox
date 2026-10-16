# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable policy tests; these do not build or link the MSVC-only UWP application."""

import base64
import hashlib
import importlib.util
import json
import re
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    Image = None

ROOT = Path(__file__).resolve().parents[2]


class AccountsTests(unittest.TestCase):
    def test_launch_policy_migration_and_avatar_path(self):
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
        self.assertGreaterEqual(len(catalogue["characters"]), 120)
        self.assertGreaterEqual(len(catalogue["colors"]), 40)
        self.assertEqual(len({c["name"] for c in catalogue["colors"]}), len(catalogue["colors"]))

    @unittest.skipIf(Image is None, "Pillow is required for PNG pixel verification")
    def test_package_catalogue_and_transparent_masters(self):
        source = (ROOT / "src/core/constants_nxbox.h").read_text()
        arrays = re.findall(r"std::array<u8, (\d+)> (NXBOX_\w+) = \{(.*?)\};", source, re.S)
        catalogue = json.loads((ROOT / "tools/nxbox-pack/src/avatar-catalog.json").read_text())
        self.assertGreaterEqual(len(catalogue["characters"]), 120)
        self.assertGreaterEqual(len(catalogue["colors"]), 40)
        self.assertEqual(len({c["name"] for c in catalogue["colors"]}), len(catalogue["colors"]))
        self.assertEqual(len(arrays), 1, "Only the NXbox logo may be embedded")
        size, name, content = arrays[0]
        self.assertEqual(name, "NXBOX_ACCOUNT_JPEG")
        data = bytes(int(value, 16) for value in re.findall(r"0x([a-f0-9]{2})", content))
        self.assertEqual(len(data), int(size))
        self.assertEqual(
            hashlib.sha256(data).hexdigest(),
            "60acf1f103b2482c1411d2e2146bef7b6310a62ba4905690655f2a45c6c4ebb1",
        )
        unique = set()
        for character in catalogue["characters"]:
            self.assertTrue(character["category"])
            asset = ROOT / "dist/nxbox/Assets/avatars" / (character["id"] + ".png")
            packaged = Image.open(asset)
            self.assertEqual(packaged.size, (256, 256))
            self.assertEqual(packaged.mode, "RGBA")
            self.assertEqual(packaged.getextrema()[3][0], 0)
            unique.add(hashlib.sha256(asset.read_bytes()).hexdigest())
            image = Image.open(ROOT / "tools/nxbox/avatars" / (character["id"] + ".png"))
            self.assertEqual(image.size, (512, 512))
            self.assertEqual(image.mode, "RGBA")
            rgba = image.resize((256, 256), Image.Resampling.LANCZOS).tobytes()
            self.assertEqual(packaged.tobytes(), rgba)
            pixels = zlib.decompress(base64.b64decode(character["rgba"]))
            self.assertEqual(len(pixels), 256 * 256 * 4)
            # Compare real translucent-edge samples, not only a synthetic fixture.
            for i in range(0, len(rgba), 4):
                self.assertEqual(pixels[i + 3], rgba[i + 3])
                for channel in range(3):
                    self.assertEqual(
                        pixels[i + channel], (rgba[i + channel] * rgba[i + 3] + 127) // 255
                    )
        self.assertEqual(len(unique), len(catalogue["characters"]))
        self.assertTrue((ROOT / "tools/nxbox/avatars/sheet.png").is_file())

    def test_single_mode_can_change_picture_without_first_launch_question(self):
        source = (ROOT / "src/eden_uwp/ui/accounts_screen.cpp").read_text()
        self.assertNotIn("Page::Question", source)
        self.assertNotIn("row_ == 2 && state_.multi", source)
        self.assertNotIn("!state_.multi || !UpdatePreview()", source)
        self.assertIn("case Page::Colors:", source)
        self.assertIn("if (!UpdatePreview())", source)
        self.assertIn("profiles_.SetAvatar(*profiles_.GetUser(account_), jpeg)", source)
        self.assertIn("row_ == 5", source)
        policy = (ROOT / "src/eden_uwp/accounts_policy.h").read_text()
        self.assertIn("return multi && launching;", policy)
        settings = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        for key in ("AccountSection", "ProfilePicture", "MultipleAccounts"):
            self.assertIn("Text::" + key, settings)

    def test_package_stages_every_runtime_portrait(self):
        spec = importlib.util.spec_from_file_location(
            "package_uwp", ROOT / "tools/nxbox/package_uwp.py"
        )
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        with tempfile.TemporaryDirectory(prefix="nxbox-avatar-package-") as temporary:
            root = Path(temporary)
            executable = root / "test.exe"
            data = bytearray(128)
            data[:2] = b"MZ"
            data[0x3C:0x40] = (64).to_bytes(4, "little")
            data[64:70] = b"PE\0\0\x64\x86"
            executable.write_bytes(data)
            destination = root / "package"
            package.stage(executable, destination, "0.1.0.0")
            catalogue = json.loads((ROOT / "tools/nxbox/avatars/catalog.json").read_text())
            for character in catalogue:
                relative = Path("Assets/avatars") / (character["id"] + ".png")
                self.assertEqual(
                    (destination / relative).read_bytes(),
                    (ROOT / "dist/nxbox" / relative).read_bytes(),
                )
            self.assertTrue((destination / "Notices/NXbox-avatars-LICENSE.md").is_file())
