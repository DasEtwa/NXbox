# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute descriptor plan reuse with the production plan and finish methods."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_subresource_feedback import function

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"

MOCK = r"""
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
using D3D12::DescriptorSources;
using D3D12::BoundHandleTableCache;
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ResourcePtr = std::shared_ptr<int>;
bool BisectEnabled(const char*) { return false; }
"""
ALLOCATOR = r"""
enum class FrameCount { DescriptorTableReuses, DescriptorCopies, DescriptorCopyCalls };
enum class FramePhase { DescriptorWrites };
struct FrameProfile { struct Scope { template<class T> Scope(T&, FramePhase) {} }; };
struct D3D12_CPU_DESCRIPTOR_HANDLE { std::uintptr_t ptr{}; };
constexpr int D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER = 0;
constexpr int D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 1;
struct DescriptorTable {
    u32 first = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    u32 Slot(u32 index) const { return first + index; }
};
struct Context {
    u64 tick = 1;
    unsigned reuses = 0, copies = 0;
    u64 CurrentTick() const { return tick; }
    Context& Profile() { return *this; }
    void Count(FrameCount count, u64 = 1) {
        if (count == FrameCount::DescriptorTableReuses) ++reuses;
    }
    Context* Device() { return this; }
    void CopyDescriptors(int, const D3D12_CPU_DESCRIPTOR_HANDLE*, const u32*, u32,
                         const D3D12_CPU_DESCRIPTOR_HANDLE*, const u32*, int) { ++copies; }

};
void TraceApiCall(Context*, const char*, void*, const void*, std::size_t) {}
struct D3D12DescriptorAllocator {
    bool descriptor_fastpath = false;
    bool handle_table_reuse = false;
    std::array<BoundHandleTableCache<DescriptorTable>, 2> handle_table_cache;
    bool descriptor_table_cache = false;
    struct DescriptorKeyHash {
        size_t operator()(const DescriptorKey& key) const { return key.kind; }
    };
    struct CachedDrawTable {
        DescriptorTable table;
        std::vector<ResourcePtr> resources;
    };
    std::array<D3D12::DescriptorSetCache<DescriptorKey, CachedDrawTable, DescriptorKeyHash>, 2>
        draw_table_cache;
    struct Cache { u64 generation = 1; } view_cache, sampler_cache;
    void RefreshCaches() {}
    struct CopyBatch {};
    struct PreviousDrawTable {
        u64 tick = 0;
        DescriptorPlan plan{{0, 0, 0}};
        DescriptorTable table;
    };
    std::array<PreviousDrawTable, 2> previous_draw_tables;
    Context context;
    u32 allocations = 0, writes = 0;
    DescriptorTable AllocateViews(u32) { return {++allocations}; }
    DescriptorTable AllocateSamplers(u32) { return {++allocations}; }
    void WriteCached(int, const ResourcePtr&, u32, const void*, u32, u32, CopyBatch*) { ++writes; }
    void FlushCopies(int, CopyBatch&) {}
    bool CanReuseDrawTable(const DescriptorPlan&, bool = false) const;
    bool CanReusePreviousDrawTable(const DescriptorPlan&, bool) const;
    const DescriptorTable* FindHandleDrawTable(const DescriptorPlan&, bool) const;
    const CachedDrawTable* FindCachedDrawTable(const DescriptorPlan&, bool) const;
    void RememberDrawTable(const DescriptorPlan&, DescriptorTable, bool);
    DescriptorTable FinishDrawTable(DescriptorPlan&, bool = false);
};
"""
DRIVER = r"""
int main() {
    D3D12DescriptorAllocator allocator;
    DescriptorPlan plan{{0, 0, 0}};
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = resource;
    auto prepare = [&](u32 kind, std::array<u32, 3> sizes = {2, 1, 1}) {
        plan.Reset(sizes);
        for (u32 i = 0; i < plan.keys.size(); ++i) {
            plan.keys[i] = {};
            plan.keys[i].kind = kind;
            plan.keys[i].resource = reinterpret_cast<std::uintptr_t>(resource.get());
            plan.resources[i] = resource;
        }
    };
    prepare(1);
    const auto first = allocator.FinishDrawTable(plan);
    assert(allocator.allocations == 1 && allocator.writes == 4);
    plan.resources.clear();
    prepare(1);
    assert(allocator.CanReuseDrawTable(plan));
    assert(allocator.FinishDrawTable(plan).first == first.first);
    assert(allocator.allocations == 1 && allocator.context.reuses == 1);
    // Both scratch and remembered buffers have now been warmed. Alternating
    // contents must replace the immutable table without discarding either buffer.
    const auto* a = plan.keys.data();
    const auto* b = allocator.previous_draw_tables[0].plan.keys.data();
    const auto* ra = plan.resources.data();
    const auto* rb = allocator.previous_draw_tables[0].plan.resources.data();
    for (u32 i = 0; i < 20; ++i) {
        prepare(2 + i % 2);
        assert(!allocator.CanReuseDrawTable(plan));
        allocator.FinishDrawTable(plan);
        assert(plan.keys.data() == (i % 2 ? a : b));
        assert(plan.resources.data() == (i % 2 ? ra : rb));
        plan.resources.clear();
    }
    // Identical slot keys with different table boundaries cannot reuse a table.
    prepare(3, {1, 2, 1});
    assert(!allocator.CanReuseDrawTable(plan));
    allocator.FinishDrawTable(plan);
    prepare(3, {1, 2, 1});
    assert(allocator.CanReuseDrawTable(plan));
    ++allocator.context.tick;
    assert(!allocator.CanReuseDrawTable(plan));
    allocator.FinishDrawTable(plan);
    prepare(3, {1, 2, 1});
    assert(!allocator.CanReuseDrawTable(plan, true));
    allocator.FinishDrawTable(plan, true);
    // Cleanup of scratch (also on skipped draws) does not release the resources
    // owned by the remembered tables. Replacing both tables releases them.
    plan.resources.clear();
    resource.reset();
    assert(!lifetime.expired());
    prepare(0, {0, 0, 0});
    allocator.FinishDrawTable(plan);
    plan.resources.clear();
    prepare(0, {0, 0, 0});
    allocator.FinishDrawTable(plan, true);
    plan.resources.clear();
    assert(lifetime.expired());
    // Execute the production bulk-copy branch, then prove that an identical
    // source array reuses only within its recording tick.
    D3D12DescriptorAllocator fast;
    fast.descriptor_fastpath = true;
    auto sources = [&] {
        plan.Reset({1, 2, 1});
        plan.sources.generation = 1;
        for (u32 i = 0; i < 4; ++i) plan.sources.handles[i] = i + 10;
    };
    sources();
    fast.FinishDrawTable(plan);
    assert(fast.context.copies == 1 && fast.writes == 0);
    sources();
    assert(fast.CanReuseDrawTable(plan));
    fast.FinishDrawTable(plan);
    assert(fast.context.copies == 1);
    ++fast.context.tick;
    assert(!fast.CanReuseDrawTable(plan));
    fast.FinishDrawTable(plan);
    assert(fast.context.copies == 2);
    // Nonconsecutive handle sets use the production bounded cache, with the
    // semantic map disabled. A changed CPU generation forces fresh GPU tables.
    D3D12DescriptorAllocator handles;
    handles.descriptor_fastpath = true;
    handles.handle_table_reuse = true;
    auto handle_build = [&](u32 kind, bool sampler = false) {
        prepare(kind, {0, 1, 0});
        plan.sources.generation = handles.view_cache.generation;
        plan.sources.handles[0] = kind + 10;
        return handles.FinishDrawTable(plan, sampler).first;
    };
    const auto handle_first = handle_build(1);
    assert(handle_build(2) != handle_first);
    assert(handle_build(1) == handle_first);
    assert(handles.allocations == 2 && handles.context.copies == 2);
    ++handles.view_cache.generation;
    assert(handle_build(1) != handle_first);
    const auto generation_first = handle_build(1);
    ++handles.context.tick;
    assert(handle_build(1) != generation_first);
    assert(handles.allocations == 4);
    assert(handle_build(1, true) != handle_first);
    // Run both production copy branches with the set cache enabled. Revisited
    // tables must avoid allocation/writes; submissions and heap domains cannot alias.
    for (bool bulk : {false, true}) {
        D3D12DescriptorAllocator cached;
        cached.descriptor_table_cache = true;
        cached.descriptor_fastpath = bulk;
        auto build = [&](u32 kind, bool sampler = false) {
            prepare(kind, {0, 1, 0});
            plan.sources.generation = 1;
            plan.sources.handles[0] = kind + 10;
            return cached.FinishDrawTable(plan, sampler).first;
        };
        const auto first_table = build(1);
        assert(build(2) != first_table);
        prepare(1, {0, 1, 0});
        plan.sources.generation = 1;
        plan.sources.handles[0] = 11;
        assert(cached.CanReuseDrawTable(plan));
        assert(cached.FinishDrawTable(plan).first == first_table);
        assert(build(1) == first_table); // Cache hit warms the previous-table fast path.
        assert(cached.allocations == 2);
        assert((bulk ? cached.context.copies : cached.writes) == 2);
        assert(build(1, true) != first_table);
        ++cached.context.tick; // Reserve submitted after its optimistic reuse check.
        assert(build(1) != first_table);
        assert(cached.allocations == 4);
        // Recycling CPU sources does not invalidate immutable GPU descriptors.
        ++cached.view_cache.generation;
        assert(build(2) != first_table);
        const auto new_first = build(1);
        assert(cached.allocations == 5);
        assert(build(1) == new_first);
        for (auto& domain : cached.draw_table_cache) domain.Clear();
        for (auto& previous : cached.previous_draw_tables) previous.tick = 0;
        assert(build(1) != new_first);
        assert(cached.allocations == 6);
    }
}
"""


class DrawScratchTests(unittest.TestCase):
    def test_plan_storage_and_tick_lifetime(self):
        header = (NATIVE / "d3d12_descriptor.h").read_text()
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        structs = header.split("struct DescriptorKey {", 1)[1].split("struct DescriptorTable {", 1)[
            0
        ]
        methods = "\n".join(
            function(source, signature)
            for signature in (
                "bool D3D12DescriptorAllocator::CanReuseDrawTable(",
                "bool D3D12DescriptorAllocator::CanReusePreviousDrawTable(",
                "const DescriptorTable* D3D12DescriptorAllocator::FindHandleDrawTable(",
                "const D3D12DescriptorAllocator::CachedDrawTable* D3D12DescriptorAllocator::FindCachedDrawTable(",
                "void D3D12DescriptorAllocator::RememberDrawTable(",
                "DescriptorTable D3D12DescriptorAllocator::FinishDrawTable(",
            )
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-draw-scratch-") as temp:
            cpp = Path(temp) / "scratch.cpp"
            binary = Path(temp) / "scratch"
            cpp.write_text(MOCK + "struct DescriptorKey {" + structs + ALLOCATOR + methods + DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-I",
                    str(ROOT / "src"),
                    "-fsanitize=address,undefined",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
