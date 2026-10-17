# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""LaunchBox metadata filtering, ranking, and portable console matching/cache policy."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile
import json

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "build_art_index", ROOT / "tools/nxbox/build_art_index.py"
)
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)
FIXTURE = ROOT / "tests/port/fixtures/launchbox-art.xml"
CASES = {
    "The Pokémon™: Adventure — Deluxe Edition": "pokemon adventure",
    "THE Legend of Zelda®: Tears of the Kingdom": "legend of zelda tears of kingdom",
    "Mario Kart 8 Digital Deluxe Edition": "mario kart 8",
    "BAYONETTA™ 3": "bayonetta 3",
    "Super Mario 3D World + Bowser’s Fury": "super mario 3d world bowser s fury",
    "星のカービィ": "星のカービィ",
    "  The Test © Remastered  ": "test",
    "Theatre 2": "theatre 2",
}


class ArtIndexTests(unittest.TestCase):
    def test_normalization(self):
        for name, expected in CASES.items():
            self.assertEqual(BUILDER.normalize(name), expected)

    def test_fixture_and_zip_cli(self):
        expected = {
            "banner only": ["only.png"],
            "pokemon adventure": ["world.jpg", "us.jpg", "europe.jpg", "japan.jpg", "banner.jpg"],
        }
        self.assertEqual(BUILDER.build_index(FIXTURE), expected)
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "Metadata.zip"
            output = Path(directory) / "index.json"
            with zipfile.ZipFile(archive, "w") as zipped:
                zipped.write(FIXTURE, "Metadata.xml")
            subprocess.run(
                [
                    "python3",
                    str(ROOT / "tools/nxbox/build_art_index.py"),
                    "--metadata",
                    str(archive),
                    "--output",
                    str(output),
                ],
                check=True,
            )
            self.assertEqual(json.loads(output.read_text()), expected)
            empty = Path(directory) / "empty.xml"
            empty.write_text("<LaunchBox/>")
            result = subprocess.run(
                [
                    "python3",
                    str(ROOT / "tools/nxbox/build_art_index.py"),
                    "--metadata",
                    str(empty),
                    "--output",
                    str(output),
                ],
                capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(json.loads(output.read_text()), expected)

    def test_console_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("portable C++ compiler unavailable")
        checks = "\n".join(
            f"assert(Normalize({json.dumps(name, ensure_ascii=False)}) == "
            f"{json.dumps(expected, ensure_ascii=False)});"
            for name, expected in CASES.items()
        )
        source = r"""
#include <cassert>
#include "eden_uwp/ui/art_index.h"
#include "eden_uwp/ui/art_cache.h"
using namespace EdenXbox::Ui::Art;
int main(int argc, char** argv) {
assert(argc == 2);
const std::filesystem::path cache = argv[1];
assert(ReplaceKeepingOld(cache, {1,2}));
assert(ReplaceKeepingOld(cache, {3,4,5}));
assert(std::filesystem::file_size(cache) == 3);
std::filesystem::create_directory(cache.string() + ".next.partial");
assert(!ReplaceKeepingOld(cache, {9}));
assert(std::filesystem::file_size(cache) == 3);
CHECKS
BackgroundIndex index{{"english game", {"best.jpg", "banner.jpg"}}, {"local game", {"local.jpg"}}};
assert(Lookup(index, "Unknown", "The English Game™").front() == "best.jpg");
assert(Lookup(index, "Local Game", "English Game").front() == "local.jpg");
assert(Lookup(index, "Missing", "").empty());
assert(SafeFilename("ABC-123_0.jpg") && !SafeFilename("../bad.jpg"));
assert(!SafeFilename("https://example.com/a.jpg") && !SafeFilename("x.jpg?redirect=x"));
assert(Fresh(100, 100) && Fresh(100, 100 + kCacheWeek - 1));
assert(!Fresh(100, 100 + kCacheWeek) && !Fresh(0, 100) && !Fresh(101, 100));
}
""".replace("CHECKS", checks)
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(source)
            subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary), str(Path(directory) / "state/index.json")], check=True)


if __name__ == "__main__":
    unittest.main()
