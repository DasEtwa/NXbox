// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>
#include <optional>

namespace D3D12 {
// Std-only ring math for descriptor heaps. Allocations are contiguous, tagged with the
// submission tick that will finish using them, and reclaimed in FIFO order by Retire().
// A request that does not fit at the end of the heap wraps to 0, and the skipped tail is
// charged to the allocation so it is reclaimed together with it.
class RingAllocator {
public:
    explicit RingAllocator(std::uint32_t capacity) : capacity{capacity} {}

    std::optional<std::uint32_t> Allocate(std::uint32_t count, std::uint64_t tick) {
        if (count == 0 || count > capacity)
            return std::nullopt;
        std::uint32_t offset{};
        std::uint32_t padding{};
        if (head >= tail && used != capacity) {
            // Free space: [head, capacity) and [0, tail).
            if (count <= capacity - head) {
                offset = head;
            } else if (count <= tail) {
                padding = capacity - head;
                offset = 0;
            } else {
                return std::nullopt;
            }
        } else if (head < tail && count <= tail - head) {
            offset = head;
        } else {
            return std::nullopt;
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
