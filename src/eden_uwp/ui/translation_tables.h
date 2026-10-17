// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <span>

#include "eden_uwp/ui/language_match.h"

namespace EdenXbox::Ui {

struct TranslationEntry {
    Text key;
    const wchar_t* value;
};

inline constexpr TranslationEntry kEnglish[] = {
#include "eden_uwp/ui/lang/en.inc"
};
inline constexpr TranslationEntry kPortuguese[] = {
#include "eden_uwp/ui/lang/pt_br.inc"
};

struct LanguageEntry {
    Language language;
    std::string_view tag;
    std::span<const TranslationEntry> table;
};

inline constexpr auto kLanguages = std::array{
#define NXBOX_LANGUAGE(id, tag, table) LanguageEntry{Language::id, tag, table},
#include "eden_uwp/ui/lang/registry.inc"
#undef NXBOX_LANGUAGE
};

constexpr const wchar_t* FindTranslation(Text key, std::span<const TranslationEntry> table) {
    for (const auto& entry : table) {
        if (entry.key == key && entry.value && entry.value[0]) {
            return entry.value;
        }
    }
    return nullptr;
}

constexpr bool CompleteEnglish() {
    std::array<bool, static_cast<std::size_t>(Text::Count)> present{};
    for (const auto& entry : kEnglish) {
        const auto key = static_cast<std::size_t>(entry.key);
        if (key >= present.size() || present[key] || !entry.value || !entry.value[0]) {
            return false;
        }
        present[key] = true;
    }
    for (const bool found : present) {
        if (!found) {
            return false;
        }
    }
    return true;
}
static_assert(CompleteEnglish(), "English must contain nonempty text for every key");

inline std::string_view LanguageTag(Language language) {
    for (const auto& entry : kLanguages) {
        if (entry.language == language) {
            return entry.tag;
        }
    }
    return "en";
}

inline const wchar_t* Translate(Text key, std::span<const TranslationEntry> table) {
    if (const auto* value = FindTranslation(key, table)) {
        return value;
    }
    if (const auto* value = FindTranslation(key, kEnglish)) {
        return value;
    }
    return L"Unknown text";
}

inline const wchar_t* Translate(Text key, Language language) {
    for (const auto& entry : kLanguages) {
        if (entry.language == language) {
            return Translate(key, entry.table);
        }
    }
    return Translate(key, kEnglish);
}

} // namespace EdenXbox::Ui
