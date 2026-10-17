// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include "core/hle/service/nextendo/runtime.h"

namespace Service::Nextendo {

inline void WriteLittleEndian(std::uint8_t* output, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
        output[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// nn::friends FriendImpl wire layout, including its 0xe0-byte presence and 0xc0-byte AppField.
inline std::array<std::uint8_t, 0x200> SerializeFriend(const FriendEntry& entry,
                                                       std::uint64_t program_id) {
    std::array<std::uint8_t, 0x200> result{};
    WriteLittleEndian(result.data(), entry.pid, 8);
    WriteLittleEndian(result.data() + 8, 0x1100000000000000ULL, 8);
    WriteLittleEndian(result.data() + 0x10, entry.pid, 8);
    auto length = (std::min)(entry.name.size(), std::size_t{32});
    // Preserve UTF-8 boundaries and reserve the nickname's terminating NUL.
    while (length < entry.name.size() && length > 0 &&
           (static_cast<unsigned char>(entry.name[length]) & 0xc0) == 0x80)
        --length;
    std::copy_n(entry.name.data(), length, result.data() + 0x18);
    std::copy_n(result.data(), 16, result.data() + 0x40);
    WriteLittleEndian(result.data() + 0x50, entry.status ? 0x7fffffffffffffffULL : 0, 8);
    WriteLittleEndian(result.data() + 0x58, entry.status, 4);
    result[0x5c] = entry.program_id != 0 && entry.program_id == program_id;
    std::copy(entry.app_field.begin(), entry.app_field.end(), result.begin() + 0x60);
    result[0x128] = entry.pid != 0;
    return result;
}

} // namespace Service::Nextendo
