// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace EdenXbox::Ui::Art {
inline constexpr std::int64_t kCacheWeek = 7 * 24 * 60 * 60;
inline bool Fresh(std::int64_t written, std::int64_t now) {
    return written > 0 && now >= written && now - written < kCacheWeek;
}
inline bool SafeFilename(const std::string& name) {
    const auto dot = name.find('.');
    if (dot == std::string::npos || dot == 0)
        return false;
    for (std::size_t i = 0; i < dot; ++i) {
        const auto c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    auto ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}
inline std::string Normalize(std::string name) {
    // UTF-8 Latin accent folding, preserving other scripts without locale-dependent ctype.
    const std::string accented[] = {"à", "á", "â", "ã", "ä", "å", "è", "é", "ê",
                                    "ë", "ì", "í", "î", "ï", "ò", "ó", "ô", "õ",
                                    "ö", "ù", "ú", "û", "ü", "ý", "ÿ", "ç", "ñ"};
    const std::string folded = "aaaaaaeeeeiiiiooooouuuuyycn";
    // Latin-1 capitals have the same UTF-8 lead byte and a continuation 32 lower.
    for (std::size_t i = 0; i + 1 < name.size(); ++i) {
        if (static_cast<unsigned char>(name[i]) == 0xc3) {
            auto c = static_cast<unsigned char>(name[i + 1]);
            if (c >= 0x80 && c <= 0x9e && c != 0x97)
                name[i + 1] = char(c + 32);
        }
    }
    for (std::size_t i = 0; i < folded.size(); ++i) {
        std::size_t at;
        while ((at = name.find(accented[i])) != std::string::npos)
            name.replace(at, accented[i].size(), 1, folded[i]);
    }
    for (const auto* mark : {"™", "®", "©"}) {
        std::size_t at;
        while ((at = name.find(mark)) != std::string::npos)
            name.erase(at, std::string(mark).size());
    }
    for (std::size_t i = 0; i + 2 < name.size(); ++i) {
        const auto a = static_cast<unsigned char>(name[i]);
        const auto b = static_cast<unsigned char>(name[i + 1]);
        const auto c = static_cast<unsigned char>(name[i + 2]);
        if ((a == 0xe2 && b >= 0x80 && b <= 0x81 && (b == 0x80 || c <= 0xaf)) ||
            (a == 0xe3 && b == 0x80 && c <= 0xbf)) {
            name.replace(i, 3, " ");
        }
    }
    for (auto& c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z')
            c = char(u + 32);
        else if (u < 128 && !((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')))
            c = ' ';
    }
    std::istringstream in(name);
    std::string word, result;
    while (in >> word) {
        if (!result.empty())
            result += ' ';
        result += word;
    }
    std::vector<std::string> suffixes = {
        "definitive edition",       "deluxe edition",     "complete edition",    "ultimate edition",
        "game of the year edition", "special edition",    "standard edition",    "gold edition",
        "digital deluxe edition",   "collectors edition", "anniversary edition", "remastered"};
    std::stable_sort(suffixes.begin(), suffixes.end(),
                     [](const auto& a, const auto& b) { return a.size() > b.size(); });
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& suffix : suffixes) {
            const auto tail = " " + suffix;
            if (result.size() > tail.size() &&
                result.compare(result.size() - tail.size(), tail.size(), tail) == 0) {
                result.resize(result.size() - tail.size());
                changed = true;
                break;
            }
        }
    }
    std::istringstream words(result);
    result.clear();
    while (words >> word) {
        if (word == "the")
            continue;
        if (!result.empty())
            result += ' ';
        result += word;
    }
    return result;
}
using BackgroundIndex = std::map<std::string, std::vector<std::string>>;
inline std::vector<std::string> Lookup(const BackgroundIndex& index, const std::string& name,
                                       const std::string& english) {
    for (const auto& title : {name, english}) {
        const auto found = index.find(Normalize(title));
        if (found != index.end() && !found->second.empty())
            return found->second;
    }
    return {};
}
} // namespace EdenXbox::Ui::Art
