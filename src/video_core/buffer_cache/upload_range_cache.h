// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace VideoCommon {

// Memoize completed CPU upload scans, never host bindings or resource ownership. A write
// invalidates its entire 4 MiB tracking region, including aliases and partial CPU pages.
// Collisions and writes to neighbouring pages only cause extra scans. Storage is fixed.
class UploadRangeCache {
    using Address = std::uint64_t;
    static constexpr unsigned RegionBits = 22;
    static constexpr Address AddressLimit = Address{1} << 34;
    static constexpr std::size_t EntryCount = 256;

    struct Entry {
        Address address{};
        Address size{};
        Address generation{};
    };

public:
    [[nodiscard]] bool Contains(Address address, Address size) const {
        if (!Valid(address, size)) {
            return false;
        }
        const auto& entry = entries[Slot(address, size)];
        return entry.generation != 0 && entry.address == address && entry.size == size &&
               entry.generation >= LastWrite(address, size);
    }

    void Record(Address address, Address size) {
        if (Valid(address, size)) {
            entries[Slot(address, size)] = {address, size, generation};
        }
    }

    void Invalidate(Address address, Address size) {
        if (!Valid(address, size)) {
            return;
        }
        AdvanceGeneration();
        const auto first = address >> RegionBits;
        const auto last = (address + size - 1) >> RegionBits;
        std::fill(writes.begin() + first, writes.begin() + last + 1, generation);
    }

    void Clear() {
        entries.fill({});
        writes.fill(0);
        generation = 1;
    }

private:
    static bool Valid(Address address, Address size) {
        return size != 0 && address < AddressLimit && size <= AddressLimit - address;
    }

    static std::size_t Slot(Address address, Address size) {
        // Mix both sub-page offsets and page numbers; aligned buffers must not all collide.
        const Address key = (address >> 4) ^ (address >> 16) ^ (size * 0x9e3779b97f4a7c15ULL);
        return (key * 0x9e3779b97f4a7c15ULL) >> 56;
    }

    Address LastWrite(Address address, Address size) const {
        const auto first = address >> RegionBits;
        const auto last = (address + size - 1) >> RegionBits;
        return *std::max_element(writes.begin() + first, writes.begin() + last + 1);
    }

    void AdvanceGeneration() {
        if (++generation == 0) {
            Clear();
        }
    }

    std::array<Entry, EntryCount> entries{};
    std::array<Address, (AddressLimit >> RegionBits)> writes{};
    Address generation{1};
};

static_assert(sizeof(UploadRangeCache) < 40 * 1024);

} // namespace VideoCommon
