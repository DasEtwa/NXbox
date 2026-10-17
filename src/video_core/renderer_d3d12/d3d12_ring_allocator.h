// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace D3D12 {
// Soft cache target: reserve space for the CPU/JIT and adapt to the actual process limit.
constexpr std::uint64_t BufferMemoryTarget(std::uint64_t limit, std::uint64_t usage,
                                           std::uint64_t cache) {
    constexpr std::uint64_t reserve = 256ULL << 20;
    const auto available = limit > usage ? limit - usage : 0;
    const auto growth = available > reserve ? available - reserve : 0;
    const auto target = limit / 2;
    return std::min(target, cache + std::min(growth, UINT64_MAX - cache));
}
// The shared cache computes fixed GC watermarks once. Charge a shrinking target
// as occupied memory so CPU/JIT growth also triggers its existing dirty-aware LRU.
constexpr std::uint64_t BufferMemoryPressure(std::uint64_t usage, std::uint64_t initial_target,
                                             std::uint64_t current_target) {
    const auto lost = initial_target > current_target ? initial_target - current_target : 0;
    return usage + std::min(lost, UINT64_MAX - usage);
}
std::uint64_t NativeBufferMemoryTarget(std::uint64_t cache) noexcept;

// Heap placement slots. Release never allocates, so completion-thread deleters stay noexcept.
class BufferHeapSlots {
public:
    static constexpr std::uint64_t Alignment = 65536;
    explicit BufferHeapSlots(std::uint32_t count) : slots(count) {}
    std::optional<std::uint32_t> Allocate(std::uint32_t count) {
        if (!count || count > slots.size())
            return std::nullopt;
        std::uint32_t run = 0;
        for (std::uint32_t i = 0; i < slots.size(); ++i) {
            run = slots[i] ? 0 : run + 1;
            if (run == count) {
                const auto start = i + 1 - count;
                std::fill_n(slots.begin() + start, count, true);
                used += count;
                return start;
            }
        }
        return std::nullopt;
    }
    void Release(std::uint32_t start, std::uint32_t count) noexcept {
        std::fill_n(slots.begin() + start, count, false);
        used -= count;
    }
    std::uint32_t Used() const noexcept {
        return used;
    }

private:
    std::vector<bool> slots;
    std::uint32_t used{};
};

// Std-only ring math for descriptor heaps. Allocations are contiguous, tagged with the
// submission tick that will finish using them, and reclaimed in FIFO order by Retire().
// A request that does not fit at the end of the heap wraps to 0, and the skipped tail is
// charged to the allocation so it is reclaimed together with it.
class RingAllocator {
public:
    explicit RingAllocator(std::uint32_t capacity) : capacity{capacity} {}

    bool CanAllocate(std::uint32_t count) const {
        if (count == 0 || count > capacity || used == capacity)
            return false;
        if (head >= tail)
            return count <= capacity - head || count <= tail;
        return count <= tail - head;
    }

    std::optional<std::uint32_t> Allocate(std::uint32_t count, std::uint64_t tick) {
        if (!CanAllocate(count))
            return std::nullopt;
        std::uint32_t offset = head;
        std::uint32_t padding{};
        if (head >= tail && count > capacity - head) {
            padding = capacity - head;
            offset = 0;
        }
        records.push_back({tick, padding ? head : offset, padding + count});
        used += padding + count;
        head = offset + count;
        return offset;
    }
    // Frees every allocation whose tick is <= completed.
    void Retire(std::uint64_t completed) {
        while (!records.empty() && records.front().tick <= completed) {
            used -= records.front().length;
            records.pop_front();
        }
        tail = records.empty() ? head : records.front().start;
        if (records.empty())
            head = tail = 0;
    }
    std::optional<std::uint64_t> OldestTick() const {
        if (records.empty())
            return std::nullopt;
        return records.front().tick;
    }
    std::uint32_t Used() const {
        return used;
    }
    std::uint32_t Capacity() const {
        return capacity;
    }

private:
    struct Record {
        std::uint64_t tick;
        std::uint32_t start;
        std::uint32_t length;
    };
    std::uint32_t capacity;
    std::uint32_t head{};
    std::uint32_t tail{};
    std::uint32_t used{};
    std::deque<Record> records;
};
} // namespace D3D12
