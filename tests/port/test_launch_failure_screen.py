# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""A failed launch shows why, then returns to the library instead of closing the app."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SESSION = (ROOT / "src/eden_uwp/game_session.cpp").read_text()


class LaunchFailureScreen(unittest.TestCase):
    def test_every_failed_launch_shows_the_failure(self):
        tail = SESSION[SESSION.index("worker.join();\n    // A failed launch says why,") :]
        tail = tail[: tail.index("\n}\n")]
        self.assertIn("if (failed && !closed.load()) {", tail)
        self.assertIn("show_failure(launch_status.failure.load());", tail)
        # Every in-app launch goes back; the next pass shows the library (allow_official_startup is
        # cleared), so a direct boot does not retry the same file. A protocol launch ends.
        self.assertIn("return_home = !protocol_launch && !closed.load();", tail)
        self.assertNotIn("if (protocol_launch && !closed.load() &&", tail)

    def test_failed_load_explains_from_the_file_content(self):
        load = SESSION[SESSION.index("const auto result = system.Load(window, path, parameters);") :]
        load = load[: load.index('Diagnostic("GAME_LOAD_FAILED status="')]
        self.assertIn("LibraryContent::LoadProblem(", load)
        self.assertIn("Ui::LibraryFileProblem(", load)
        self.assertIn("launch_status.failure.store(LibraryContent::Label(problem));", load)

if __name__ == "__main__":
    unittest.main()
