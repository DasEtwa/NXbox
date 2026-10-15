// SPDX-License-Identifier: GPL-3.0-or-later
// Host stubs surround production methods inserted by test_gpu_frontend_hot_paths.py.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "common/scope_exit.h"

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using DAddr = u64;
using PAddr = u64;
namespace Memory {
constexpr size_t YUZU_PAGEBITS = 12;
constexpr size_t YUZU_PAGESIZE = 1 << YUZU_PAGEBITS;
constexpr size_t YUZU_PAGEMASK = YUZU_PAGESIZE - 1;
} // namespace Memory

template <typename Traits>
struct DeviceMemoryManager {
    static constexpr size_t page_bits = Memory::YUZU_PAGEBITS;
    static constexpr size_t page_mask = Memory::YUZU_PAGEMASK;
    struct Entry {
        u32 continuity_tracker{};
        u32 compressed_physical_ptr{};
    };
    struct Entries {
        std::array<Entry, 16> values{};
        mutable unsigned lookups{};
        const Entry& operator[](size_t index) const {
            ++lookups;
            return values.at(index);
        }
    } tracked_entries;
    std::array<u8, 16 * Memory::YUZU_PAGESIZE> physical{};
    template <typename T>
    T* GetPointerFromRaw(PAddr address) {
        return reinterpret_cast<T*>(physical.data() + address);
    }
    template <typename T>
    const T* GetPointerFromRaw(PAddr address) const {
        return reinterpret_cast<const T*>(physical.data() + address);
    }
    u8* GetSpan(DAddr, size_t);
    const u8* GetSpan(DAddr, size_t) const;
    void WalkBlock(DAddr, size_t, auto, auto, auto);
};

// METHODS

int main() {
    DeviceMemoryManager<void> memory;
    const auto& view = memory;
    constexpr auto page = Memory::YUZU_PAGESIZE;
    memory.tracked_entries.values[1] = {2, 5}; // Three contiguous pages at physical page 4.
    memory.tracked_entries.values[2] = {1, 6};
    memory.tracked_entries.values[3] = {0, 7};
    memory.tracked_entries.values[4] = {0, 0}; // Unmapped, including nonzero continuity metadata.
    memory.tracked_entries.values[5] = {1, 0};
    memory.tracked_entries.values[6] = {0, 0};
    memory.tracked_entries.values[7] = {0, 11}; // Discontinuous physical backing.
    for (size_t i = 0; i < memory.physical.size(); ++i)
        memory.physical[i] = static_cast<u8>((i * 37 + i / page) % 251);

    for (size_t offset : {0U, 1U, 4095U}) {
        for (size_t size : {size_t{0}, page - offset, 3 * page - offset, 3 * page - offset + 1}) {
            memory.tracked_entries.lookups = 0;
            auto* span = memory.GetSpan(page + offset, size);
            assert(memory.tracked_entries.lookups == 1);
            const auto* expected =
                size <= 3 * page - offset ? memory.physical.data() + 4 * page + offset : nullptr;
            assert(span == expected);
            memory.tracked_entries.lookups = 0;
            assert(view.GetSpan(page + offset, size) == expected);
            assert(memory.tracked_entries.lookups == 1);
        }
    }
    assert(memory.GetSpan(4 * page, 1) == nullptr);
    assert(view.GetSpan(5 * page, page) == nullptr);
    assert(memory.GetSpan(7 * page + page - 1, 1) == memory.physical.data() + 11 * page - 1);
    assert(view.GetSpan(7 * page + page - 1, 2) == nullptr);

    // Walk a partial first page, contiguous mapped pages, holes, and a final mapped page.
    // Compare every copied/zero-filled byte against an independent page-by-page oracle.
    for (size_t offset : {0U, 3U, 4095U}) {
        const DAddr start = page + offset;
        const size_t size = 7 * page - offset - 17;
        std::vector<u8> result(size, 255), expected(size);
        for (size_t i = 0; i < size; ++i) {
            const auto address = start + i;
            const auto& entry = memory.tracked_entries.values[address / page];
            if (entry.compressed_physical_ptr)
                expected[i] =
                    memory.physical[(entry.compressed_physical_ptr - 1) * page + address % page];
        }
        size_t position = 0, unmapped = 0, mapped = 0;
        memory.tracked_entries.lookups = 0;
        memory.WalkBlock(
            start, size,
            [&](size_t count, DAddr address) {
                assert(address == start + position);
                std::memset(result.data() + position, 0, count);
                ++unmapped;
            },
            [&](size_t count, const u8* pointer) {
                std::memcpy(result.data() + position, pointer, count);
                ++mapped;
            },
            [&](size_t count) { position += count; });
        assert(result == expected && position == size);
        assert(mapped == 2 && unmapped == 2);
        assert(memory.tracked_entries.lookups == 4);

        // The same walk backs writes; holes are skipped and mapped chunks retain offsets.
        std::vector<u8> written = result;
        std::fill(written.begin(), written.end(), 42);
        position = 0;
        memory.WalkBlock(
            start, size, [](size_t, DAddr) {},
            [&](size_t count, u8* pointer) {
                std::memcpy(pointer, written.data() + position, count);
            },
            [&](size_t count) { position += count; });
        for (size_t i = 0; i < size; ++i) {
            const auto address = start + i;
            const auto& entry = memory.tracked_entries.values[address / page];
            if (entry.compressed_physical_ptr)
                assert(
                    memory.physical[(entry.compressed_physical_ptr - 1) * page + address % page] ==
                    42);
        }
    }
    memory.tracked_entries.lookups = 0;
    memory.WalkBlock(
        0, 0, [](size_t, DAddr) { assert(false); }, [](size_t, u8*) { assert(false); },
        [](size_t) { assert(false); });
    assert(memory.tracked_entries.lookups == 0);
}
