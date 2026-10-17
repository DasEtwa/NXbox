// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>

namespace D3D12::IndexConversion {
// Hash the input bytes separately; never hash this struct's padding.
struct CacheKey {
    std::uint64_t address{}, content_hash{};
    std::uint32_t count{}, first{}, format{}, topology{}, restart_index{}, provoking{};
    bool restart_enabled{};
    std::uint64_t device_address{}; // Validation must distinguish guest remaps within one buffer.
    bool operator==(const CacheKey&) const = default;
};
struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        std::size_t result{};
        const auto mix = [&](std::uint64_t value) {
            result ^= static_cast<std::size_t>(value) + 0x9e3779b9u + (result << 6) + (result >> 2);
        };
        mix(key.address);
        mix(key.content_hash);
        mix(key.count);
        mix(key.first);
        mix(key.format);
        mix(key.topology);
        mix(key.restart_index);
        mix(key.provoking);
        mix(key.restart_enabled);
        mix(key.device_address);
        return result;
    }
};
// Identity plus write generation avoids hashing immutable input again. Weak identity
// rejects allocator/address reuse without keeping guest buffer resources alive.
template <typename Resource, typename Value>
struct ValidatedSnapshot {
    std::weak_ptr<Resource> source;
    std::uint64_t version{};
    Value value;
    bool Matches(const std::shared_ptr<Resource>& current) const {
        return current && source.lock() == current && version == current->content_version;
    }
};
// Values own immutable allocations. Eviction drops only the cache's reference;
// the renderer must retain every bound allocation until its submission fence retires.
template <typename Value>
class ConversionCache {
public:
    explicit ConversionCache(std::size_t budget = 64 * 1024 * 1024) : budget{budget} {}
    ConversionCache(const ConversionCache&) = delete;
    ConversionCache& operator=(const ConversionCache&) = delete;

    Value* Find(const CacheKey& key) {
        const auto found = lookup.find(key);
        if (found == lookup.end())
            return nullptr;
        entries.splice(entries.begin(), entries, found->second);
        return &found->second->value;
    }
    void Insert(const CacheKey& key, Value value, std::size_t bytes) {
        if (const auto found = lookup.find(key); found != lookup.end())
            Erase(found->second);
        // Zero-sized results have no allocation and must not grow unbounded metadata.
        if (!bytes || bytes > budget)
            return;
        while (used > budget - bytes || entries.size() >= 4096)
            Erase(std::prev(entries.end()));
        entries.push_front({key, std::move(value), bytes});
        try {
            lookup.emplace(key, entries.begin());
        } catch (...) {
            entries.pop_front();
            throw;
        }
        used += bytes;
    }
    std::size_t Bytes() const {
        return used;
    }
    std::size_t Size() const {
        return entries.size();
    }

private:
    struct Entry {
        CacheKey key;
        Value value;
        std::size_t bytes;
    };
    using Iterator = typename std::list<Entry>::iterator;
    void Erase(Iterator entry) {
        used -= entry->bytes;
        lookup.erase(entry->key);
        entries.erase(entry);
    }
    std::size_t budget, used{};
    std::list<Entry> entries;
    std::unordered_map<CacheKey, Iterator, CacheKeyHash> lookup;
};
} // namespace D3D12::IndexConversion
