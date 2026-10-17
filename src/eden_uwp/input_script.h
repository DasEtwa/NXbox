// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace EdenXbox {
/// One scripted press: `name` is held from `start` for `hold` seconds.
struct ScriptEvent {
    double start;
    double hold;
    std::string name;
};

/// Parses "40.0:A:0.15,45:PLUS:0.15" (seconds since game start : button : hold seconds). Entries
/// that do not parse are skipped. Names are upper-cased; hold defaults to 0.15 s when omitted.
inline std::vector<ScriptEvent> ParseInputScript(const std::string& text) {
    std::vector<ScriptEvent> events;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto end = text.find(',', pos);
        if (end == std::string::npos)
            end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        const auto first = item.find(':');
        if (first == std::string::npos)
            continue;
        const auto second = item.find(':', first + 1);
        const std::string start_text = item.substr(0, first);
        std::string name = item.substr(first + 1, second == std::string::npos
                                                      ? std::string::npos
                                                      : second - first - 1);
        const std::string hold_text =
            second == std::string::npos ? "0.15" : item.substr(second + 1);
        char* tail = nullptr;
        const double start = std::strtod(start_text.c_str(), &tail);
        if (start_text.empty() || *tail != '\0' || start < 0)
            continue;
        const double hold = std::strtod(hold_text.c_str(), &tail);
        if (hold_text.empty() || *tail != '\0' || hold <= 0)
            continue;
        name.erase(0, name.find_first_not_of(' '));
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name.empty())
            continue;
        for (auto& c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        events.push_back({start, hold, name});
    }
    return events;
}

inline bool ScriptEventActive(const ScriptEvent& event, double now) {
    return now >= event.start && now < event.start + event.hold;
}
} // namespace EdenXbox
