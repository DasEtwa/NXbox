# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""A game saved under a removable drive letter that changed is found on the drive it moved to."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SESSION = (ROOT / "src/eden_uwp/game_session.cpp").read_text()


class GameMovedDrive(unittest.TestCase):
    def test_missing_game_is_looked_up_on_the_other_drives_first(self):
        resolve = SESSION[SESSION.index("std::string ResolveGamePath(") :]
        resolve = resolve[: resolve.index("\n}\n")]
        moved = resolve.index("FindOnRemovableDrives(target)")
        missing = resolve.index('Diagnostic("GAME_MISSING "')
        self.assertLess(moved, missing)
        self.assertIn('Diagnostic("GAME_MOVED "', resolve)

    def test_lookup_keeps_the_path_below_nxbox_games(self):
        finder = SESSION[SESSION.index("FindOnRemovableDrives(const std::filesystem::path& missing)") :]
        finder = finder[: finder.index("\n}\n")]
        self.assertIn('L"\\\\nxbox\\\\games\\\\"', finder)
        self.assertIn("ExternalGameFolders()", finder)
        self.assertIn("Common::FS::IsFile(candidate)", finder)


if __name__ == "__main__":
    unittest.main()
