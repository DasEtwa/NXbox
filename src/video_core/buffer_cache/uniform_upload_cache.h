// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace VideoCommon {

// CPU shadows of completed uploads, not GPU readbacks or upload-allocator pointers.
// Exact equality avoids hash collisions. Bounded to 1 MiB, allocated on demand.
class UniformUploadCache {
public:
    static bool Enabled(std::uint64_t program_id, const char* option) {
        return program_id == 0x01007EF00011E000ULL && (!option || std::string_view{option} != "0");
    }

    bool MatchAndRecord(std::uint64_t address, std::span<const std::uint8_t> bytes) {
        for (const auto& entry : entries) {
            if (entry.address == address && entry.bytes.size() == bytes.size() && !bytes.empty() &&
                std::equal(bytes.begin(), bytes.end(), entry.bytes.begin())) {
                return true;
            }
        }
        // Invalidate overlapping shadows even when this upload is too large to retain.
        Invalidate(address, bytes.size());
        if (!bytes.empty() && bytes.size() <= MaxBytes) {
            auto& entry = entries[next++ % entries.size()];
            populated = true;
            entry.address = address;
            entry.bytes.reserve(MaxBytes);
            entry.bytes.assign(bytes.begin(), bytes.end());
        }
        return false;
    }

    void Invalidate(std::uint64_t address, std::uint64_t size) {
        if (!populated || !size) {
            return;
        }
        for (auto& entry : entries) {
            // Subtraction avoids overflow for ranges ending at the address-space limit.
            if (!entry.bytes.empty() &&
                (address <= entry.address ? entry.address - address < size
                                          : address - entry.address < entry.bytes.size())) {
                entry.bytes.clear();
            }
        }
    }

private:
    static constexpr std::size_t MaxBytes = 64 * 1024;
    struct Entry {
        std::uint64_t address{};
        std::vector<std::uint8_t> bytes;
    };
    std::array<Entry, 16> entries{};
    std::size_t next{};
    bool populated{};
};

} // namespace VideoCommon
