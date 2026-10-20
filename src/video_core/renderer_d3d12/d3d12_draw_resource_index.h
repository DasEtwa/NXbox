// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace D3D12 {

// Index the ordered resource/state vector without owning resources or allocating.
// Hash collisions always compare resource identity. A full index falls back to
// the original scan, so unusually large draws keep their exact merge behavior.
template <std::size_t Capacity = 128>
class DrawResourceIndex {
public:
    DrawResourceIndex() {
        Reset();
    }

    void Reset() {
        indexed = false;
        full = false;
    }

    template <class Entries>
    static std::size_t FindLinear(const void* resource, const Entries& entries) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].first.get() == resource)
                return i;
        }
        return entries.size();
    }

    // A result equal to entries.size() reserves that index. The caller must
    // append the resource before the next lookup, or Reset after aborting a draw.
    // Indices survive vector relocation; raw pointers into its storage do not.
    template <class Entries>
    std::size_t FindOrInsert(const void* resource, const Entries& entries) {
        if (!indexed) {
            // Tiny draws are cheaper to scan. Initialize lazily so they also
            // avoid clearing the table, including aborted state collection.
            if (entries.size() <= LinearLimit || full || entries.size() > Capacity)
                return FindLinear(resource, entries);
            slots.fill(Empty);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                auto bucket = Bucket(entries[i].first.get());
                while (slots[bucket] != Empty)
                    bucket = (bucket + 1) & (Capacity - 1);
                slots[bucket] = i;
            }
            indexed = true;
        }
        const auto hash = Bucket(resource);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            auto& index = slots[(hash + probe) & (Capacity - 1)];
            if (index == Empty) {
                index = entries.size();
                return index;
            }
            if (entries[index].first.get() == resource)
                return index;
        }
        indexed = false;
        full = true;
        return FindLinear(resource, entries);
    }

private:
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);
    static constexpr std::size_t Empty = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t LinearLimit = Capacity >= 32 ? 8 : 0;
    static std::size_t Bucket(const void* resource) {
        const auto address = reinterpret_cast<std::uintptr_t>(resource);
        // Resource objects are aligned; mix upper bits to spread regular strides.
        return ((address >> 4) ^ (address >> 13)) & (Capacity - 1);
    }
    std::array<std::size_t, Capacity> slots;
    bool indexed{};
    bool full{};
};

} // namespace D3D12
