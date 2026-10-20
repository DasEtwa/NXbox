// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <iostream>
#include <memory>
#include "video_core/renderer_d3d12/d3d12_descriptor.h"

struct CollisionHash {
    std::size_t operator()(unsigned) const {
        return 0;
    }
};
struct Table {
    unsigned address;
    std::shared_ptr<int> resource;
};

int main() {
    D3D12::DescriptorSetCache<unsigned, Table, CollisionHash> cache;
    const std::array<std::uint32_t, 3> layout{0, 2, 1};
    const std::vector<unsigned> a{11, 22, 33}, b{11, 44, 33};
    auto resource = std::make_shared<int>(42);
    const std::weak_ptr<int> lifetime = resource;
    unsigned copies{};
    auto draw = [&](std::uint64_t tick, const auto& sizes, const auto& keys) {
        if (const auto* hit = cache.Find(tick, sizes, keys))
            return hit->address;
        const unsigned address = ++copies;
        cache.Remember(tick, sizes, keys, Table{address, resource});
        return address;
    };
    assert(draw(1, layout, a) == 1);
    assert(draw(1, layout, b) == 2);
    assert(draw(1, layout, a) == 1); // A -> B -> A avoids all copies on the hit.
    assert(copies == 2);
    assert(!cache.Find(1, {1, 1, 1}, a)); // Same slots, different root-table slices.
    assert(!cache.Find(1, layout, std::vector<unsigned>{22, 11, 33}));
    assert(!cache.Find(1, layout, std::vector<unsigned>{11, 22}));
    resource.reset();
    assert(!lifetime.expired());       // Semantic keys cannot outlive their resource identity.
    assert(!cache.Find(2, layout, a)); // Reserve/submit invalidates the GPU slots.
    assert(draw(2, layout, a) == 3);
    assert(lifetime.expired()); // Old recording's resource pins are released.
    cache.Clear();              // Frame boundary can occur without a different submission tick.
    assert(!cache.Find(2, layout, a));
    assert(draw(2, layout, a) == 4);
    assert(copies == 4);
    std::cout << "descriptor tables: collision-safe A/B/A reuse, layout/order/count, "
                 "submission/frame invalidation and resource lifetime passed; copies=4\n";
}
