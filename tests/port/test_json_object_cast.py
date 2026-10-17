# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class JsonObjectCastTest(unittest.TestCase):
    def test_json_values_use_get_object(self):
        # QueryInterface from IJsonValue to JsonObject fails ("No such interface supported"), which
        # silently dropped every saved source on the console. GetObject() is the conversion.
        pattern = re.compile(r"\.as<(?:winrt::Windows::Data::Json::)?Json(?:Object|Array)>\(\)")
        offenders = [
            f"{path.relative_to(ROOT)}:{n}"
            for path in (ROOT / "src/eden_uwp").rglob("*.[ch]*")
            for n, line in enumerate(path.read_text(errors="ignore").splitlines(), 1)
            if pattern.search(line)
        ]
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
