// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace NszFailure {

enum class Kind { Damaged, Other };

inline bool Unchanged(std::uint64_t saved_size, std::int64_t saved_mtime, std::uint64_t size,
                      std::int64_t mtime) {
    return saved_size == size && saved_mtime == mtime;
}

inline bool ShouldRemember(std::string_view failure, bool cancelled) {
    return !cancelled && !failure.empty() && failure != "cancelled" &&
           failure.find("disk full") == std::string_view::npos;
}

inline Kind Classify(std::string_view failure) {
    std::string text(failure);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // NCA hash verification currently only warns; size mismatches are conversion failures.
    if (text.find("corrupt") != std::string::npos || text.find("mismatch") != std::string::npos ||
        text.find("does not match") != std::string::npos ||
        (text.find("bytes of the nca instead of") != std::string::npos)) {
        return Kind::Damaged;
    }
    return Kind::Other;
}

} // namespace NszFailure
