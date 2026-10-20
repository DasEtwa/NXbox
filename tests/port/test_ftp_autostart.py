# SPDX-License-Identifier: GPL-3.0-or-later
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class FtpAutostartTests(unittest.TestCase):
    def test_flag_file_starts_server_with_library(self):
        source = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        ctor = source.split("LibraryScreen(Renderer& renderer", 1)[1].split("std::string Run()", 1)[0]
        self.assertIn('exists(local_state_ / "ftp-autostart")', ctor)
        self.assertIn("ftp_.Start(local_state_)", ctor)


if __name__ == "__main__":
    unittest.main()
