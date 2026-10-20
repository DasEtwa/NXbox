# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the recorder's production direct replay path and its scratch opt-out."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name("d3d12_direct_replay.cpp")


class DirectReplay(unittest.TestCase):
    def run_binary(self, sanitizer=None):
        with tempfile.TemporaryDirectory(prefix="d3d12-direct-replay-") as tmp:
            binary = Path(tmp) / "test"
            command = [
                "clang++",
                "-std=c++20",
                "-O2" if sanitizer else "-O3",
                "-pthread",
                "-I" + str(ROOT / "src"),
                str(SOURCE),
                "-o",
                str(binary),
            ]
            if sanitizer:
                command += ["-g", "-fsanitize=" + sanitizer]
            subprocess.run(command, check=True, capture_output=True, text=True)
            result = subprocess.run(
                [str(binary)] + ([] if sanitizer else ["benchmark"]),
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(result.stderr, result.stderr)
            print(result.stdout, end="")

    def test_address_undefined(self):
        self.run_binary("address,undefined")

    def test_thread(self):
        self.run_binary("thread")

    def test_benchmark(self):
        self.run_binary()


if __name__ == "__main__":
    unittest.main()
