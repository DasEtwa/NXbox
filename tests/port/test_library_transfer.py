# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise relative transfer destinations and the existing copy safety scenarios."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class LibraryTransfer(unittest.TestCase):
    def test_relative_paths_and_transfer_guards(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "transfer"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/library_transfer.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_scan_and_copy_create_directories(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        self.assertIn("ExternalGameFolders(false)", library)
        self.assertIn('{"Base", "Updates", "DLC"}', library)
        self.assertIn("Common::FS::CreateDirs(file.target.parent_path())", library)
        self.assertIn(
            "LibraryTransfer::Target(candidate.path, internal.front(), destination)", library
        )
        usb = (ROOT / "src/eden_uwp/usb_library.cpp").read_text()
        self.assertIn("current.GetFoldersAsync()", usb)

    def test_delete_enumerates_removable_subfolders_with_guards(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        candidates = library.split("LibraryDeleteCandidates(", 1)[1].split(
            "bool DeleteLibraryFiles(", 1
        )[0]
        self.assertIn("index < folders.size()", candidates)
        self.assertIn("storage->GetFoldersAsync()", candidates)
        self.assertIn("LibraryFiles::Allowed(path, roots)", candidates)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", candidates)
        self.assertIn("folders.push_back(path)", candidates)
        self.assertLess(
            candidates.index("if (!SafeDeletePath(path, roots))"),
            candidates.index("if (sibling || related)"),
        )
