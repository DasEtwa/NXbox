# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise NSZ failure retry decisions, classification and scan wiring."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NszFailure(unittest.TestCase):
    def test_retry_and_classification(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "nsz_failure"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/nsz_failure.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_scan_wiring(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        previous = library.split("std::optional<std::string> PreviousFailure", 1)[1].split(
            "void RememberFailure", 1
        )[0]
        self.assertIn("NszFailure::Unchanged(", previous)
        self.assertIn("ModifiedSeconds(source)", previous)
        self.assertIn("fs::remove(record_path, ec)", previous)
        scan = library.split("void LibraryScan::ConvertCompressed", 1)[1].split(
            "std::vector<GameEntry> LibraryScan::Scan", 1
        )[0]
        self.assertLess(scan.index("PreviousFailure("), scan.index("pending.emplace_back"))
        self.assertIn("NszFailure::Classify(*failure)", scan)
        self.assertIn("NszFailure::Classify(failure)", scan)
        guard = "else if (NszFailure::ShouldRemember(failure, cancel_.load()))"
        self.assertIn(guard, scan)
        self.assertLess(scan.index(guard), scan.index("RememberFailure("))
        # Only damaged files are remembered; environmental failures retry on the next scan.
        damaged_only = "if (kind == NszFailure::Kind::Damaged)\n                    RememberFailure("
        self.assertIn(damaged_only, scan)
        self.assertIn("source_size,", scan)
        self.assertIn("source_mtime, failure", scan)
        self.assertIn("fs::remove(FailureRecordPath(local_state_, nsz), ec)", scan)
        converter = (ROOT / "src/core/file_sys/nsz.cpp").read_text()
        self.assertIn('Fail("cancelled")', converter)

    def test_scan_toast_and_translations(self):
        screen = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        adoption = screen.split("void AdoptScan()", 1)[1].split("scan_.reset()", 1)[0]
        self.assertIn("scan_->FailedConversions()", adoption)
        self.assertIn("failures.front()", adoption)
        self.assertIn("failures.size() - 1", adoption)
        self.assertIn("Toast(message, Clock::now())", adoption)
        for language in ("pt_br", "en"):
            table = (ROOT / f"src/eden_uwp/ui/lang/{language}.inc").read_text()
            for key in ("ConversionDamaged", "ConversionFailed"):
                self.assertIn(f"Text::{key}", table)
