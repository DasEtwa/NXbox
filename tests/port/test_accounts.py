# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable policy tests; these do not build or link the MSVC-only UWP application."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

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

    def test_embedded_avatars_are_distinct_256_jpegs(self):
        source = (ROOT / "src/core/constants_nxbox.h").read_text()
        arrays = re.findall(r"std::array<u8, (\d+)> (NXBOX_\w+) = \{(.*?)\};", source, re.S)
        self.assertEqual(len(arrays), 21)
        unique = set()
        for size, name, content in arrays:
            with self.subTest(avatar=name):
                data = bytes(int(value, 16) for value in re.findall(r"0x([a-f0-9]{2})", content))
                self.assertEqual(len(data), int(size))
                self.assertLessEqual(len(data), 0x20000)
                self.assertTrue(data.startswith(b"\xff\xd8"))
                self.assertTrue(data.endswith(b"\xff\xd9"))
                sof = data.index(b"\xff\xc0")
                self.assertEqual(data[sof + 5 : sof + 9], b"\x01\x00\x01\x00")
                unique.add(data)
        self.assertEqual(len(unique), 21)
