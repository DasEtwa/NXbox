// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

namespace D3D12 {
// CBVs return zero outside their declared extent. Own just the final alignment padding.
constexpr std::uint32_t UniformViewSize(std::uint32_t size) {
    return size && size <= 65536 ? (size + 255) & ~255u : 0;
}

// Immutable snapshots, keyed by source identity and write generation. Weak source leases
// prevent the cache from keeping guest buffers alive; GPU command lists retain evictions.
template <typename Resource, std::size_t Capacity = 128>
class UniformCopyCache {
public:
    std::shared_ptr<Resource> Find(const std::shared_ptr<Resource>& source, std::uint64_t version,
                                   std::uint32_t offset, std::uint32_t size) {
        std::erase_if(entries, [](const auto& entry) { return entry.source.expired(); });
        for (const auto& entry : entries) {
            if (entry.source.lock() == source && entry.version == version &&
                entry.offset == offset && entry.size == size)
                return entry.copy;
        }
        return {};
    }
    void Insert(const std::shared_ptr<Resource>& source, std::uint64_t version,
                std::uint32_t offset, std::uint32_t size, std::shared_ptr<Resource> copy) {
        // Replace stale snapshots for this range rather than filling the cache with versions.
        std::erase_if(entries, [&](const auto& entry) {
            return entry.source.expired() ||
                   (entry.source.lock() == source && entry.offset == offset && entry.size == size);
        });
        if (entries.size() == Capacity)
            entries.pop_front();
        entries.push_back({source, version, offset, size, std::move(copy)});
    }

private:
    static_assert(Capacity > 0);
    struct Entry {
        std::weak_ptr<Resource> source;
        std::uint64_t version;
        std::uint32_t offset;
        std::uint32_t size;
        std::shared_ptr<Resource> copy;
    };
    std::deque<Entry> entries;
};
} // namespace D3D12
