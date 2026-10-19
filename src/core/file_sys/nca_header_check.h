// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
namespace FileSys {
// A fixed-size raw section sample detects preallocated holes without reading the payload.
inline bool ZeroNcaSectionHeader(std::span<const unsigned char> bytes) {
    return bytes.size() == 16 &&
           std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0; });
}
inline std::optional<std::uint64_t> NcaSectionProbeOffset(std::uint64_t section,
                                                          std::uint64_t section_size,
                                                          std::int64_t target,
                                                          std::uint64_t file_size) {
    if (target < 0 || section > file_size || section_size > file_size - section ||
        section_size < 16 || static_cast<std::uint64_t>(target) > section_size - 16)
        return std::nullopt;
    return section + static_cast<std::uint64_t>(target);
}
} // namespace FileSys
