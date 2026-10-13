"""SystemSettings is 0x336A0 bytes and the Xbox fiber stacks are 512 KiB. Building the defaults by
value on first boot (no settings.dat yet) overflowed the set:sys service fiber and killed every
fresh install before the first frame, so the defaults must be filled in place on the heap."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SET = ROOT / "src/core/hle/service/set"


class SettingsDefaultsOffStack(unittest.TestCase):
    def test_system_defaults_fill_in_place(self):
        header = (SET / "setting_formats/system_settings.h").read_text()
        self.assertIn("void DefaultSystemSettings(SystemSettings& settings);", header)
        self.assertNotRegex(header, r"\bSystemSettings\s+DefaultSystemSettings\s*\(")
        source = (SET / "setting_formats/system_settings.cpp").read_text()
        self.assertNotRegex(source, r"\bSystemSettings\s+\w+\s*(\{|;|=)")
        self.assertNotIn("settings = {}", source)

    def test_loader_keeps_defaults_on_the_heap(self):
        server = (SET / "system_settings_server.cpp").read_text()
        self.assertIn("decltype(default_func())::element_type", server)
        self.assertIn("std::make_unique<SystemSettings>()", server)
        self.assertEqual(re.findall(r"return Default\w+Settings\(\);", server), [])


if __name__ == "__main__":
    unittest.main()
