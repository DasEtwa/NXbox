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
