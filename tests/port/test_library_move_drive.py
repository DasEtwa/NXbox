# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Move to HD picks the removable drive with the most free space, not the first one found."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MoveDriveTests(unittest.TestCase):
    def test_destination_is_the_roomiest_drive(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        body = library.split("LibraryTransfer::Result MoveLibraryGame(", 1)[1].split("\n}\n", 1)[0]
        self.assertNotIn("folders.front()", body)
        choice = body[body.index("for (const auto& folder : folders)") :]
        self.assertIn("FreeSpace(folder).value_or(0)", choice)
        self.assertIn("free > most_free", choice)
        # The choice happens before any file is planned against the destination.
        self.assertLess(body.index("free > most_free"), body.index("LibraryTransfer::Target("))


if __name__ == "__main__":
    unittest.main()
