# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the shipped language-mod classifier with real-world naming patterns."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ModLanguageTests(unittest.TestCase):
    def test_language_audio_subcategories_and_unrelated_skins(self):
        source = (ROOT / "src/eden_uwp/ui/mods.cpp").read_text()
        classifier = source.split("bool ContainsAny(", 1)[1].split("struct LanguageInfo", 1)[0]
        driver = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <string>
#include <vector>
struct ModEntry { std::string name; std::string category; };
std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
bool ContainsAny(CLASSIFIER
int main() {
    assert(IsTranslationEntry({"Brazilian Portuguese Audio", "Audio"}));
    assert(IsTranslationEntry({"Português brasileiro", "Other"}));
    assert(IsTranslationEntry({"French pack", "Audio"}));
    assert(IsTranslationEntry({"English localization", "Other"}));
    assert(IsTranslationEntry({"Alternate dialogue", "Audio Voice"}));
    assert(IsTranslationEntry({"Example game", "Translation"}));
    assert(!IsTranslationEntry({"Luigi costume", "Skins"}));
    assert(!IsTranslationEntry({"Frankenstein", "Skins"}));
}
'''.replace("CLASSIFIER", classifier)
        with tempfile.TemporaryDirectory(prefix="nxbox-mod-language-") as temporary:
            path = Path(temporary)
            (path / "test.cpp").write_text(driver)
            subprocess.run(
                ["clang++", "-std=c++20", "-fno-rtti", "-Wall", "-Wextra", "-Wshadow", "-Werror",
                 str(path / "test.cpp"), "-o", str(path / "test")],
                check=True, capture_output=True, text=True, timeout=60,
            )
            subprocess.run([str(path / "test")], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
