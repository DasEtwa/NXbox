# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Join games that NXbox Pack uploaded as .nxpartNNN slices plus a .nxparts manifest."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIB = 1024 * 1024
DRIVER = r"""
#include <iostream>
#include "frontend_common/split_upload.h"
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    SplitUpload::Log log;
    if (argc == 2) {
        log = [](const std::string& line) { std::cout << line << '\n'; };
    }
    const auto result = SplitUpload::JoinSplitUploads(argv[1], log);
    std::cout << "RESULT joined=" << result.joined << " skipped=" << result.skipped << '\n';
    return 0;
}
"""


def make_split(folder: Path, name: str, chunks: list[bytes]) -> bytes:
    """Lay a game out the way NXbox Pack leaves it on the console; returns the original bytes."""
    folder.mkdir(parents=True, exist_ok=True)
    for index, chunk in enumerate(chunks):
        (folder / f"{name}.nxpart{index:03d}").write_bytes(chunk)
    whole = b"".join(chunks)
    (folder / f"{name}.nxparts").write_text(f"parts={len(chunks)}\nsize={len(whole)}\n")
    return whole


def listing(folder: Path) -> set[str]:
    return {entry.name for entry in folder.iterdir()}


class SplitUploadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxbox-split-build-")
        build = Path(cls.build.name)
        (build / "driver.cpp").write_text(DRIVER)
        cls.binaries = {}
        for standard in ("c++20", "c++17"):
            binary = build / f"driver-{standard.replace('+', 'p')}"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    f"-std={standard}",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(build / "driver.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            cls.binaries[standard] = binary

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nxbox-split-")
        self.addCleanup(self.temp.cleanup)
        self.games = Path(self.temp.name) / "games"
        self.games.mkdir()

    def join(self, *extra: str, standard: str = "c++20") -> list[str]:
        done = subprocess.run(
            [str(self.binaries[standard]), str(self.games), *extra],
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        )
        return done.stdout.splitlines()

    def test_three_parts_join_to_the_original_bytes(self):
        for standard in self.binaries:
            with self.subTest(standard=standard):
                folder = self.games / standard
                # Parts larger than the 4 MiB copy buffer, and one that is not a multiple of it.
                chunks = [os.urandom(5 * MIB + 3), os.urandom(4 * MIB), os.urandom(17)]
                whole = make_split(folder, "base.nsp", chunks)
                # A second game in a subfolder, and an orphan part with no manifest.
                nested = make_split(folder / "sub" / "deeper", "dlc.nsp", [b"ab", b"cd", b"e"])
                (folder / "orphan.nsp.nxpart000").write_bytes(b"keep")
                lines = self.join(standard=standard)
                self.assertEqual((folder / "base.nsp").read_bytes(), whole)
                self.assertEqual((folder / "sub/deeper/dlc.nsp").read_bytes(), nested)
                self.assertEqual(listing(folder), {"base.nsp", "orphan.nsp.nxpart000", "sub"})
                self.assertEqual(listing(folder / "sub/deeper"), {"dlc.nsp"})
                self.assertIn(f"NXPARTS_JOINED base.nsp {len(whole) >> 20}", lines)
                self.assertIn("NXPARTS_JOINED dlc.nsp 0", lines)
                self.assertEqual(lines[-1], "RESULT joined=2 skipped=0")

    def test_interrupted_join_resumes_from_the_parts_on_disk(self):
        # Part 0 was appended and deleted, then part 1 was only half appended.
        chunks = [os.urandom(MIB), os.urandom(2 * MIB + 5), os.urandom(MIB + 1)]
        folder = self.games / "half"
        whole = make_split(folder, "base.nsp", chunks)
        (folder / "base.nsp.nxpart000").unlink()
        (folder / "base.nsp").write_bytes(chunks[0] + chunks[1][: MIB + 7])
        # Part 1 was appended whole but never deleted, so the target is a part too long.
        full = self.games / "full"
        other = make_split(full, "base.nsp", chunks)
        (full / "base.nsp.nxpart000").unlink()
        (full / "base.nsp").write_bytes(chunks[0] + chunks[1])
        lines = self.join()
        self.assertEqual((folder / "base.nsp").read_bytes(), whole)
        self.assertEqual((full / "base.nsp").read_bytes(), other)
        self.assertEqual(listing(folder), {"base.nsp"})
        self.assertEqual(listing(full), {"base.nsp"})
        self.assertEqual(lines[-1], "RESULT joined=2 skipped=0")

    def test_target_smaller_than_expected_is_broken_and_nothing_is_deleted(self):
        chunks = [os.urandom(1000), os.urandom(1000), os.urandom(500)]
        short = self.games / "short"
        make_split(short, "base.nsp", chunks)
        (short / "base.nsp.nxpart000").unlink()
        (short / "base.nsp").write_bytes(chunks[0][:400])
        missing = self.games / "missing"
        make_split(missing, "base.nsp", chunks)
        (missing / "base.nsp.nxpart000").unlink()
        before = {path: path.read_bytes() for path in self.games.rglob("*") if path.is_file()}
        lines = self.join()
        self.assertIn("NXPARTS_BROKEN base.nsp", lines)
        self.assertEqual(lines[-1], "RESULT joined=0 skipped=2")
        after = {path: path.read_bytes() for path in self.games.rglob("*") if path.is_file()}
        self.assertEqual(after, before)
        self.assertFalse((missing / "base.nsp").exists())

    def test_a_hole_in_the_remaining_parts_is_broken(self):
        folder = self.games / "hole"
        make_split(folder, "base.nsp", [b"aa", b"bb", b"cc"])
        (folder / "base.nsp.nxpart001").unlink()
        lines = self.join()
        self.assertTrue(any(line.startswith("NXPARTS_BROKEN base.nsp") for line in lines))
        self.assertEqual(
            listing(folder), {"base.nsp.nxpart000", "base.nsp.nxpart002", "base.nsp.nxparts"}
        )
        self.assertFalse((folder / "base.nsp").exists())

    def test_bad_manifests_are_broken(self):
        for index, text in enumerate(
            ["", "parts=2\n", "size=3\n", "parts=x\nsize=3\n", "parts=99999\nsize=1\n"]
        ):
            folder = self.games / str(index)
            folder.mkdir()
            (folder / "base.nsp.nxparts").write_text(text)
        lines = self.join()
        self.assertEqual(lines.count("NXPARTS_BROKEN base.nsp"), 5)
        self.assertEqual(lines[-1], "RESULT joined=0 skipped=5")

    def test_manifest_with_full_target_and_no_parts_is_removed(self):
        (self.games / "base.nsp").write_bytes(b"0123456789")
        (self.games / "base.nsp.nxparts").write_text("parts=2\nsize=10\n")
        lines = self.join()
        self.assertEqual(listing(self.games), {"base.nsp"})
        self.assertEqual((self.games / "base.nsp").read_bytes(), b"0123456789")
        self.assertEqual(lines[-1], "RESULT joined=1 skipped=0")

    def test_not_enough_free_space_leaves_everything_for_the_next_start(self):
        huge = 8 * 1024**4
        part = self.games / "base.nsp.nxpart000"
        try:
            with part.open("wb") as handle:
                handle.truncate(huge)
        except OSError:
            self.skipTest("the temp filesystem cannot hold an 8 TiB sparse file")
        (self.games / "base.nsp.nxparts").write_text(f"parts=1\nsize={huge}\n")
        lines = self.join()
        self.assertTrue(
            any(re.fullmatch(r"NXPARTS_NO_SPACE base.nsp need=8388608 free=\d+", l) for l in lines),
            lines,
        )
        self.assertTrue(part.exists() and (self.games / "base.nsp.nxparts").exists())
        self.assertEqual(lines[-1], "RESULT joined=0 skipped=1")

    def test_missing_folder_and_missing_log_are_harmless(self):
        subprocess.run(
            [str(self.binaries["c++20"]), str(self.games / "nope")],
            check=True,
            capture_output=True,
            timeout=30,
        )
        make_split(self.games, "base.nsp", [b"x", b"y"])
        self.join("nolog")
        self.assertEqual((self.games / "base.nsp").read_bytes(), b"xy")

    def test_the_console_calls_the_join_before_it_looks_for_games(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        scan = library[library.index("std::vector<GameEntry> LibraryScan::Scan()") :]
        self.assertIn('#include "frontend_common/split_upload.h"', library)
        self.assertRegex(scan, r"JoinSplitUploads\(games_dir,")
        self.assertLess(scan.index("JoinSplitUploads("), scan.index("directory_iterator iterator"))
        # Part files must stay invisible to the scan: it only keeps these extensions.
        self.assertIn('extension == L".nsp" || extension == L".xci"', scan)

        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        resolve = session[session.index("std::string ResolveGamePath(") :]
        resolve = resolve[: resolve.index("\n}\n")]
        self.assertIn('#include "frontend_common/split_upload.h"', session)
        self.assertRegex(resolve, r"JoinSplitUploads\(target\.parent_path\(\),")
        self.assertLess(
            resolve.index("JoinSplitUploads("), resolve.index("if (!Common::FS::IsFile(target)) {")
        )


if __name__ == "__main__":
    unittest.main()
