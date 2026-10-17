# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate community translation tables and exercise the portable UI language resolver."""

import ast
import re
import subprocess
import tempfile
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LANG = ROOT / "src/eden_uwp/ui/lang"
ENTRY = re.compile(r'\{Text::(\w+),\s*((?:L"(?:[^"\\]|\\.)*"\s*)+)\},')
PLACEHOLDER = re.compile(
    r"%%|%(?:\d+\$)?[-+#0 ']*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]|\{[A-Za-z_][A-Za-z_0-9]*\}"
)


def placeholders(text):
    return Counter(token for token in PLACEHOLDER.findall(text) if token != "%%")


def read_table(path):
    entries = ENTRY.findall(path.read_text())
    keys = [key for key, _ in entries]
    if len(keys) != len(set(keys)):
        raise ValueError(f"Duplicate translation key in {path}")
    return {
        key: ast.literal_eval(re.sub(r'L(?=")', "", value).replace("\n", " "))
        for key, value in entries
    }


DRIVER = r"""
#include <cassert>
#include <string>
#include <vector>
#include "eden_uwp/ui/translation_tables.h"
using namespace EdenXbox::Ui;
int main() {
    assert(MatchPreferences(std::vector<std::string>{"pt-PT"}, kLanguages) == Language::Portuguese);
    assert(MatchPreferences(std::vector<std::string>{"pt-BR"}, kLanguages) == Language::Portuguese);
    assert(MatchPreferences(std::vector<std::string>{"unknown", "PT_br.UTF-8"}, kLanguages) == Language::Portuguese);
    assert(MatchPreferences(std::vector<std::string>{"ptx", "es-ES", "zh-Hans-CN"}, kLanguages) == Language::English);
    assert(!MatchLanguage("", kLanguages));
    const auto spanish = static_cast<Language>(100);
    const auto chinese = static_cast<Language>(101);
    const auto portugal = static_cast<Language>(102);
    const auto registry = std::array{
        LanguageEntry{Language::English, "en", kEnglish},
        LanguageEntry{Language::Portuguese, "pt-BR", kPortuguese},
        LanguageEntry{portugal, "pt-PT", kEnglish},
        LanguageEntry{spanish, "es", kEnglish},
        LanguageEntry{chinese, "zh-Hans", kEnglish},
    };
    for (const auto& [tag, expected] : std::array{
        std::pair{"pt-PT", portugal}, std::pair{"pt-BR", Language::Portuguese},
        std::pair{"es-ES", spanish}, std::pair{"es-MX", spanish},
        std::pair{"zh-Hans-CN", chinese}, std::pair{"unknown", Language::English}}) {
        assert(MatchPreferences(std::vector<std::string>{tag}, registry) == expected);
    }
    assert(MatchPreferences(std::vector<std::string>{"unknown", "es-MX", "pt-BR"}, registry) == spanish);
    const TranslationEntry partial[] = {{Text::NavLibrary, L"Translated"}, {Text::NavSettings, L""}};
    assert(std::wstring(Translate(Text::NavLibrary, partial)) == L"Translated");
    assert(std::wstring(Translate(Text::NavSettings, partial)) == L"Settings");
    assert(std::wstring(Translate(Text::ActionPlay, partial)) == L"Play");
    for (const auto& language : kLanguages) {
        for (int key = 0; key < static_cast<int>(Text::Count); ++key) {
            assert(Translate(static_cast<Text>(key), language.language)[0]);
        }
    }
}
"""


class TranslationTests(unittest.TestCase):
    def test_known_keys_and_placeholders(self):
        english = read_table(LANG / "en.inc")
        enum = (
            (ROOT / "src/eden_uwp/ui/strings.h")
            .read_text()
            .split("enum class Text {")[1]
            .split("};")[0]
        )
        keys = set(re.findall(r"^    (\w+),", enum, re.MULTILINE)) - {"Count"}
        self.assertEqual(set(english), keys)
        self.assertTrue(all(english.values()))
        for path in LANG.glob("*.inc"):
            if path.name in {"en.inc", "registry.inc"}:
                continue
            with self.subTest(language=path.stem):
                table = read_table(path)
                self.assertLessEqual(set(table), keys)
                for key, value in table.items():
                    self.assertEqual(placeholders(value), placeholders(english[key]), key)

    def test_placeholder_validation_preserves_counts_and_names(self):
        self.assertEqual(placeholders("%s {name} %s %%"), Counter({"%s": 2, "{name}": 1}))
        self.assertNotEqual(placeholders("%s {name}"), placeholders("%d {other}"))

    def test_matching_and_fallback(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-i18n-") as directory:
            driver = Path(directory) / "driver.cpp"
            driver.write_text(DRIVER)
            binary = Path(directory) / "driver"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(driver),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
