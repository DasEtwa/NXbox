// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/translation_tables.h"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>

#include "eden_uwp/diagnostic.h"

namespace EdenXbox::Ui {
namespace {

// nxbox_env.txt is only applied to the environment once Mesa starts, which is after this screen,
// so the setting is also read straight from the file.
std::string ForcedLanguage() {
    if (const char* value = std::getenv("NXBOX_LANG")) {
        return value;
    }
    try {
        const std::filesystem::path file =
            std::filesystem::path(std::wstring_view(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
            "nxbox_env.txt";
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            constexpr std::string_view key = "NXBOX_LANG=";
            if (line.starts_with(key)) {
                return line.substr(key.size());
            }
        }
    } catch (const winrt::hresult_error&) {
        // No LocalFolder to read: fall back to the system language.
    }
    return {};
}

Language ResolveLanguage() {
    const std::string forced = ForcedLanguage();
    if (const auto match = MatchLanguage(forced, kLanguages)) {
        Diagnostic("UI language " + std::string(LanguageTag(match->language)) + " (NXBOX_LANG)");
        return match->language;
    }
    try {
        const auto languages =
            winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages();
        std::vector<std::string> preferences;
        for (const auto& tag : languages) {
            preferences.push_back(winrt::to_string(tag));
        }
        const Language language = MatchPreferences(preferences, kLanguages);
        Diagnostic("UI language " + std::string(LanguageTag(language)) + " (system)");
        return language;
    } catch (const winrt::hresult_error&) {
        // Preference list unavailable: English below.
    }
    Diagnostic("UI language en (default)");
    return Language::English;
}

} // namespace

Language CurrentLanguage() {
    static const Language language = ResolveLanguage();
    return language;
}

const wchar_t* Tr(Text text) {
    return Translate(text, CurrentLanguage());
}

} // namespace EdenXbox::Ui
