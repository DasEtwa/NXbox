# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""A failed launch shows why: from the library it returns there instead of closing the app."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SESSION = (ROOT / "src/eden_uwp/game_session.cpp").read_text()


class LaunchFailureScreen(unittest.TestCase):
    def test_every_failed_launch_shows_the_failure(self):
        tail = SESSION[SESSION.index("worker.join();\n    // A failed launch says why.") :]
        tail = tail[: tail.index("\n}\n")]
        self.assertIn("!(failed && protocol_launch)", tail)
        self.assertIn("if (failed && !closed.load()) {", tail)
        self.assertNotIn("if (protocol_launch && !closed.load() &&", tail)

    def test_missing_exefs_gets_its_own_text(self):
        load = SESSION[SESSION.index("const auto result = system.Load(window, path, parameters);") :]
        load = load[: load.index('Diagnostic("GAME_LOAD_FAILED status="')]
        self.assertIn("u32(Loader::ResultStatus::ErrorNoExeFS)", load)
        self.assertIn("launch_status.missing_exefs.store(", load)
        self.assertIn("Ui::Text::LaunchNeedsUpdate", SESSION)
        for lang in ("en.inc", "pt_br.inc"):
            with self.subTest(lang=lang):
                self.assertIn("{Text::LaunchNeedsUpdate,", (ROOT / "src/eden_uwp/ui/lang" / lang).read_text())


if __name__ == "__main__":
    unittest.main()
