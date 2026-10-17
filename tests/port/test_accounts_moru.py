# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Moru mapping and real service database tests, without compiling the UWP app."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MoruTests(unittest.TestCase):
    def test_service_round_trip_crc_cap_and_art(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("A host C++20 compiler is required")
        candidates = [
            Path(os.environ.get("FMT_INCLUDE_DIR", "/usr/include")),
            Path("/opt/homebrew/include"),
            Path("/usr/local/include"),
        ]
        fmt = next((p for p in candidates if (p / "fmt/format.h").exists()), None)
        if fmt is None:
            self.skipTest("fmt headers required by production UUID code (set FMT_INCLUDE_DIR)")
        with tempfile.TemporaryDirectory(prefix="nxbox-moru-") as directory:
            temp = Path(directory)
            logging = temp / "common/logging.h"
            logging.parent.mkdir()
            logging.write_text(
                "#pragma once\n"
                "#define LOG_ERROR(...) ((void)0)\n"
                "#define LOG_INFO(...) ((void)0)\n"
                "#define LOG_CRITICAL(...) ((void)0)\n"
            )
            sources = [
                "tests/port/moru_data.cpp",
                "src/common/uuid.cpp",
                "src/common/random.cpp",
                "src/common/fs/file.cpp",
                "src/core/hle/service/mii/types/core_data.cpp",
                "src/core/hle/service/mii/types/store_data.cpp",
                "src/core/hle/service/mii/types/char_info.cpp",
                "src/core/hle/service/mii/types/raw_data.cpp",
                "src/core/hle/service/mii/mii_database.cpp",
                "src/core/hle/service/mii/mii_database_manager.cpp",
            ]
            command = [
                compiler,
                "-std=c++20",
                "-O1",
                "-DFMT_HEADER_ONLY",
                "-include",
                "cstring",
                "-I",
                str(temp),
                "-I",
                str(ROOT / "src"),
                "-I",
                str(fmt),
                *[str(ROOT / s) for s in sources],
                "-o",
                str(temp / "moru"),
            ]
            result = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(temp / "moru"), str(temp / "nand")], check=True)

    def test_no_reserved_name_in_visible_strings(self):
        paths = [
            ROOT / "src/eden_uwp/ui/strings.h",
            *sorted((ROOT / "src/eden_uwp/ui/lang").glob("*.inc")),
        ]
        for path in paths:
            self.assertIsNone(re.search("mii", path.read_text(), re.IGNORECASE), str(path))

    def test_translations_complete(self):
        source = (ROOT / "src/eden_uwp/ui/moru_screen.cpp").read_text()
        keys = set(re.findall(r"Text::(Moru\w+)", source))
        for language in ("en", "pt_br"):
            translated = (ROOT / f"src/eden_uwp/ui/lang/{language}.inc").read_text()
            for key in keys:
                self.assertIn(f"Text::{key},", translated)
