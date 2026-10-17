// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <span>
#include <string>
#include <string_view>

#include "eden_uwp/ui/strings.h"

namespace EdenXbox::Ui {

inline std::string NormalizeLanguageTag(std::string_view tag) {
    std::string normalized;
    for (const unsigned char letter : tag) {
        if (letter == '.' || letter == '@') {
            break;
        }
        normalized += letter == '_' ? '-' : static_cast<char>(std::tolower(letter));
    }
    return normalized;
}

inline std::string_view PrimarySubtag(std::string_view tag) {
    return tag.substr(0, tag.find('-'));
}

// Exact tag first, then a registered primary tag, then the first regional variant.
// Registry order determines which regional variant serves an unregistered region.
template <typename Registry>
const typename Registry::value_type* MatchLanguage(std::string_view preference,
                                                   const Registry& registry) {
    const std::string tag = NormalizeLanguageTag(preference);
    if (tag.empty()) {
        return nullptr;
    }
    for (const auto& entry : registry) {
        if (NormalizeLanguageTag(entry.tag) == tag) {
            return &entry;
        }
    }
    const auto primary = PrimarySubtag(tag);
    for (const auto& entry : registry) {
        if (NormalizeLanguageTag(entry.tag) == primary) {
            return &entry;
        }
    }
    for (const auto& entry : registry) {
        if (PrimarySubtag(NormalizeLanguageTag(entry.tag)) == primary) {
            return &entry;
        }
    }
    return nullptr;
}

template <typename Preferences, typename Registry>
Language MatchPreferences(const Preferences& preferences, const Registry& registry) {
    for (const auto& tag : preferences) {
        if (const auto* match = MatchLanguage(tag, registry)) {
            return match->language;
        }
    }
    return Language::English;
}

} // namespace EdenXbox::Ui
