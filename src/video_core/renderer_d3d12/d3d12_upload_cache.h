// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

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
    UniformCopyCache() = default;
    void Clear() {
        lookup.clear();
        entries.clear();
    }
    UniformCopyCache(const UniformCopyCache&) = delete;
    UniformCopyCache& operator=(const UniformCopyCache&) = delete;
    std::shared_ptr<Resource> Find(const std::shared_ptr<Resource>& source, std::uint64_t version,
                                   std::uint32_t offset, std::uint32_t size,
                                   std::uint32_t* copy_offset = nullptr) {
        if (!source)
            return {};
        const auto found = lookup.find({source.get(), offset, size});
        if (found == lookup.end())
            return {};
        const auto entry = found->second;
        // A raw address in the key is only a lookup hint, never an identity lease.
        if (entry->source.lock() != source || entry->version != version)
            return {};
        entries.splice(entries.begin(), entries, entry);
        if (copy_offset)
            *copy_offset = entry->copy_offset;
        return entry->copy;
    }
    void Insert(const std::shared_ptr<Resource>& source, std::uint64_t version,
                std::uint32_t offset, std::uint32_t size, std::shared_ptr<Resource> copy,
                std::uint32_t copy_offset = 0) {
        if (!source)
            return;
        const Key key{source.get(), offset, size};
        if (const auto found = lookup.find(key); found != lookup.end()) {
            const auto entry = found->second;
            entry->source = source;
            entry->version = version;
            entry->copy = std::move(copy);
            entry->copy_offset = copy_offset;
            entries.splice(entries.begin(), entries, entry);
            return;
        }
        if (entries.size() == Capacity) {
            lookup.erase(entries.back().key);
            entries.pop_back();
        }
        entries.push_front({key, source, version, std::move(copy), copy_offset});
        try {
            lookup.emplace(key, entries.begin());
        } catch (...) {
            entries.pop_front();
            throw;
        }
    }

private:
    static_assert(Capacity > 0);
    struct Key {
        Resource* source;
        std::uint32_t offset, size;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            auto hash = std::hash<Resource*>{}(key.source);
            for (const auto value : {key.offset, key.size})
                hash ^= std::size_t{value} + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    struct Entry {
        Key key;
        std::weak_ptr<Resource> source;
        std::uint64_t version;
        std::shared_ptr<Resource> copy;
        std::uint32_t copy_offset{};
    };
    std::list<Entry> entries;
    std::unordered_map<Key, typename std::list<Entry>::iterator, KeyHash> lookup;
};
} // namespace D3D12
