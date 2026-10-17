# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual mods JSON accessor with std-only wrapped-value mocks."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/eden_uwp/ui/mods.cpp"

DRIVER = r"""
#include <cassert>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

enum class JsonValueType { Null, String, Object };
struct JsonObject;
struct IJsonValue {
    JsonValueType type = JsonValueType::Null;
    std::shared_ptr<JsonObject> object;
    JsonValueType ValueType() const { return type; }
    JsonObject GetObject() const;
    // A wrapper is not the object it contains. QueryInterface must fail.
    template <typename T> T as() const { throw std::runtime_error("E_NOINTERFACE"); }
};
struct JsonObject {
    std::map<std::wstring, IJsonValue> fields;
    bool HasKey(const wchar_t* key) const { return fields.count(key) != 0; }
    IJsonValue GetNamedValue(const wchar_t* key) const { return fields.at(key); }
};
JsonObject IJsonValue::GetObject() const {
    assert(type == JsonValueType::Object && object);
    return *object;
}
namespace winrt::Windows::Data::Json { using JsonObject = ::JsonObject; }

// Inserted verbatim from mods.cpp, so the old interface cast fails at runtime.
ACCESSOR

int main() {
    const JsonObject item{{{L"link", {JsonValueType::String, {}}}}};
    const JsonObject section{{{L"Example game", {JsonValueType::Object,
                                               std::make_shared<JsonObject>(item)}}}};
    const JsonObject root{{{L"translations", {JsonValueType::Object,
                                             std::make_shared<JsonObject>(section)}},
                           {L"modifications", {JsonValueType::Null, {}}},
                           {L"wrong_type", {JsonValueType::String, {}}}}};
    const auto translations = ObjectAt(root, L"translations");
    assert(translations.fields.size() == 1);
    assert(ObjectAt(translations, L"Example game").HasKey(L"link"));
    assert(ObjectAt(root, L"missing").fields.empty());
    assert(ObjectAt(root, L"modifications").fields.empty());
    assert(ObjectAt(root, L"wrong_type").fields.empty());

    const JsonObject records{{{L"_aMetadata", {JsonValueType::Object,
                                              std::make_shared<JsonObject>(item)}},
                              {L"_aPreviewMedia", {JsonValueType::Object,
                                                  std::make_shared<JsonObject>(item)}}}};
    assert(ObjectAt(records, L"_aMetadata").HasKey(L"link"));
    assert(ObjectAt(records, L"_aPreviewMedia").HasKey(L"link"));
}
"""


class ModsJsonTests(unittest.TestCase):
    def test_wrapped_objects_and_missing_fields(self):
        source = SOURCE.read_text()
        accessor = re.search(r"JsonObject ObjectAt\([^\n]+\) \{.*?\n\}", source, re.DOTALL).group()
        with tempfile.TemporaryDirectory(prefix="nxbox-mods-json-") as temporary:
            driver = Path(temporary) / "driver.cpp"
            binary = Path(temporary) / "driver"
            driver.write_text(DRIVER.replace("ACCESSOR", accessor))
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(driver),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_all_mods_object_values_are_unwrapped(self):
        # Array records, images, installed records and files have the same wrapper semantics.
        self.assertNotRegex(SOURCE.read_text(), r"\.as\s*<[^>]*JsonObject\s*>")


if __name__ == "__main__":
    unittest.main()
