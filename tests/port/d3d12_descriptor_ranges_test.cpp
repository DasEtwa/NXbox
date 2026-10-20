// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_descriptor.h"

#include <cassert>
#include <iostream>
#include <random>

struct Handle {
    std::uintptr_t ptr;
};

int main() {
    std::mt19937 random{0xD312};
    std::size_t input_ranges{}, output_ranges{};
    for (unsigned replay = 0; replay < 10000; ++replay) {
        D3D12::DescriptorCopyRanges<Handle, 128> ranges;
        std::vector<std::uintptr_t> handles;
        const auto count = random() % 129;
        for (unsigned i = 0; i < count; ++i) {
            const auto address = i && random() % 4 ? handles.back() + 32 : random() % 32 * 32;
            handles.push_back(address);
            ranges.Add({address}, 32);
        }
        std::vector<std::uintptr_t> expanded;
        for (std::size_t range = 0; range < ranges.count; ++range)
            for (unsigned offset = 0; offset < ranges.sizes[range]; ++offset)
                expanded.push_back(ranges.starts[range].ptr + offset * 32);
        assert(expanded == handles);
        input_ranges += count;
        output_ranges += ranges.count;
    }
    assert(output_ranges < input_ranges / 2);
    D3D12::DescriptorCopyRanges<Handle, 4> repeated;
    for (unsigned i = 0; i < 4; ++i)
        repeated.Add({128}, 32);
    assert(repeated.count == 4); // Null sources must fill every destination slot.
    D3D12::DescriptorCopyRanges<Handle, 4> disabled;
    for (unsigned i = 0; i < 4; ++i)
        disabled.Add({i * 32}, 0);
    assert(disabled.count == 4);
    D3D12::DescriptorCopyRanges<Handle, 4> separate_heaps;
    separate_heaps.Add({32}, 32, 1);
    separate_heaps.Add({64}, 32, 2);
    separate_heaps.Add({96}, 32, 2);
    assert(separate_heaps.count == 2 && separate_heaps.sizes[1] == 2);

    // All keys collide in one set. Four ways retain recurring A/B/C/D tables
    // that the old direct-mapped cache discarded on each intervening draw.
    D3D12::BoundHandleTableCache<unsigned, 1, 1, 4> tables;
    const std::array<std::uint32_t, 3> sizes{1, 0, 0};
    for (std::uintptr_t i = 0; i < 4; ++i)
        tables.Remember(1, 1, sizes, std::array{i}, static_cast<unsigned>(i + 100));
    for (std::uintptr_t i = 0; i < 4; ++i) {
        assert(*tables.Find(1, 1, sizes, std::array{i}) == i + 100);
        assert(!tables.Find(2, 1, sizes, std::array{i}));
        assert(!tables.Find(1, 2, sizes, std::array{i}));
        assert(!tables.Find(1, 1, {0, 1, 0}, std::array{i}));
    }
    tables.Remember(1, 1, sizes, std::array<std::uintptr_t, 1>{4}, 104);
    assert(!tables.Find(1, 1, sizes, std::array<std::uintptr_t, 1>{0}));
    assert(*tables.Find(1, 1, sizes, std::array<std::uintptr_t, 1>{4}) == 104);
    tables.Remember(1, 1, sizes, std::array<std::uintptr_t, 1>{4}, 204);
    assert(*tables.Find(1, 1, sizes, std::array<std::uintptr_t, 1>{4}) == 204);
    tables.Clear();
    assert(!tables.Find(1, 1, sizes, std::array<std::uintptr_t, 1>{4}));
    std::cout << "10000 range replays: " << input_ranges << " -> " << output_ranges
              << " ranges; identical ordered copies; four colliding tables retained\n";
}
