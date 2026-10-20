# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production barrier paths with recording-only host mocks."""

import unittest

from test_d3d12_draw_bookkeeping import compile_run
from test_d3d12_subresource_feedback import MOCK, NATIVE, function


class GpuBarrierTests(unittest.TestCase):
    def test_private_query_barriers_are_scoped_batched_and_bisectable(self):
        source = (NATIVE / "d3d12_query_cache.cpp").read_text()
        method = function(source, "void QueryUavBarrier(")
        mock = MOCK.replace(
            "struct D3D12_RESOURCE_BARRIER {",
            "struct D3D12_RESOURCE_BARRIER { struct { ID3D12Resource* pResource{}; } UAV;",
        ).replace(
            "FrameProfile profile;",
            "FrameProfile profile; FrameProfile& Profile() { return profile; } "
            "void GlobalUavBarrier() { D3D12_RESOURCE_BARRIER b{}; "
            "b.Type=2; Commands()->ResourceBarrier(1, &b); }",
        )
        compile_run(
            mock
            + r"""
#include <string_view>
constexpr unsigned D3D12_RESOURCE_BARRIER_TYPE_UAV = 2;
bool global_query{};
bool BisectEnabled(const char* name) {
    assert(std::string_view{name} == "NXBOX_D3D12_GLOBAL_QUERY_UAV");
    return global_query;
}
"""
            + method
            + r"""
int main() {
    Context context;
    auto counters = std::make_shared<Resource>();
    auto report = std::make_shared<Resource>();
    QueryUavBarrier(context, counters, report);
    assert(context.recording.calls.size() == 1);
    const auto& batch = context.recording.calls.back();
    assert(batch.size() == 2);
    assert(batch[0].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV);
    assert(batch[1].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV);
    assert(batch[0].UAV.pResource == counters->handle.Get());
    assert(batch[1].UAV.pResource == report->handle.Get());
    global_query = true;
    QueryUavBarrier(context, counters, report);
    assert(context.recording.calls.size() == 2);
    assert(context.recording.calls.back().size() == 1);
    assert(context.recording.calls.back()[0].UAV.pResource == nullptr);
    // Batched report output is ordered by its COPY_SOURCE transition instead.
    global_query = false;
    QueryUavBarrier(context, counters, {});
    assert(context.recording.calls.size() == 3);
    assert(context.recording.calls.back().size() == 1);
    assert(context.recording.calls.back()[0].UAV.pResource == counters->handle.Get());
}
"""
        )
        self.assertEqual(source.count("QueryUavBarrier(context, gpu_counters, gpu_report);"), 2)
        self.assertEqual(
            source.count("QueryUavBarrier(context, impl->gpu_counters, impl->predicate);"), 2
        )

    def test_dxc_defaults_to_o3_without_debug_information(self):
        compiler = function(
            (NATIVE / "d3d12_shader.cpp").read_text(), "std::vector<u8> ShaderCompiler::Compile("
        )
        self.assertIn('L"-O3"', compiler)
        self.assertIn("optimization_flags[DxcOptimization()]", compiler)
        for flag in ('L"-Zi"', 'L"-Zs"', 'L"-Qembed_debug"', 'L"-Od"'):
            self.assertNotIn(flag, compiler)
        # Diagnostic optimization overrides are already tested by test_d3d12_pso_normalize.
        policy = (NATIVE / "d3d12_compile_policy.h").read_text()
        self.assertIn('ParseDxcOptimization(value ? value : "")', policy)

    def test_shader_read_subset_keeps_actual_state_and_write_dependencies(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        methods = "\n".join(
            function(source, f"void Context::{name}(")
            for name in ("Transition", "TransitionBatch", "TransitionSubresources")
        )
        compile_run(
            MOCK
            + methods
            + r"""
int main() {
    Context context;
    auto resource = std::make_shared<Resource>();
    resource->state = SrvState;
    context.Transition(resource, 64);
    assert(context.recording.calls.empty());
    assert(resource->state == SrvState);
    const std::pair<ResourcePtr, unsigned> reads[] = {{resource, 128}, {resource, 64}};
    context.TransitionBatch(reads);
    assert(context.recording.calls.empty() && resource->state == SrvState);
    context.Transition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(context.recording.calls.back()[0].Transition.StateBefore == SrvState);
    assert(resource->content_version == 1);
    context.Transition(resource, 64);
    context.Transition(resource, 128); // A disjoint read state still needs a transition.
    assert(context.recording.calls.back()[0].Transition.StateBefore == 64);
    assert(context.recording.calls.back()[0].Transition.StateAfter == 128);
    context.Transition(resource, 0); // COMMON is not an empty read subset.
    assert(context.recording.calls.back()[0].Transition.StateAfter == 0);
    resource->state = SrvState | D3D12_RESOURCE_STATE_RENDER_TARGET;
    context.TransitionBatch(reads); // Never reuse a state containing a write bit.
    assert(context.recording.calls.back()[0].Transition.StateBefore ==
           (SrvState | D3D12_RESOURCE_STATE_RENDER_TARGET));
    resource->state = SrvState;
    const std::pair<ResourcePtr, unsigned> mixed[] = {
        {resource, 64}, {resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}, {resource, 128}
    };
    context.TransitionBatch(mixed);
    const auto& barriers = context.recording.calls.back();
    assert(barriers.size() == 2);
    assert(barriers[0].Transition.StateBefore == SrvState);
    assert(barriers[0].Transition.StateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(barriers[1].Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(barriers[1].Transition.StateAfter == 128);
    assert(resource->content_version == 2);
}
"""
        )

    def test_scoped_uav_barriers_deduplicate_and_skip_read_only_dispatches(self):
        source = (NATIVE / "d3d12_dispatch.cpp").read_text()
        method = function(source, "void ComputeDispatch::UavBarrier(")
        compile_run(
            r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
using UINT = unsigned;
constexpr unsigned D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 8;
constexpr unsigned D3D12_RESOURCE_BARRIER_TYPE_UAV = 2;
struct ID3D12Resource {};
struct Handle { ID3D12Resource* value; ID3D12Resource* Get() const { return value; } };
struct Resource { Handle handle; };
using ResourcePtr = std::shared_ptr<Resource>;
struct ComputeResourceUse { ResourcePtr resource; unsigned state; };
struct ComputeBindings { std::span<const ComputeResourceUse> resources; };
struct D3D12_RESOURCE_BARRIER {
    unsigned Type{};
    struct { ID3D12Resource* pResource{}; } UAV;
};
enum class FramePhase { Barriers };
enum class FrameCount { Barriers };
struct FrameProfile {
    unsigned count{};
    struct Scope { Scope(FrameProfile&, FramePhase) {} };
    void Count(FrameCount, std::size_t size = 1) { count += static_cast<unsigned>(size); }
};
bool scoped{};
bool BisectEnabled(const char* name) {
    assert(std::string_view{name} == "NXBOX_D3D12_SCOPED_COMPUTE_UAV");
    return scoped;
}
struct CommandsMock {
    std::vector<std::vector<D3D12_RESOURCE_BARRIER>> calls;
    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
        assert(count);
        calls.emplace_back(barriers, barriers + count);
    }
};
struct Context {
    FrameProfile profile;
    CommandsMock commands;
    FrameProfile& Profile() { return profile; }
    CommandsMock* Commands() { return &commands; }
};
struct ComputeDispatch {
    Context& context;
    void UavBarrier(const ComputeBindings&);
};
"""
            + method
            + r"""
int main() {
    Context context;
    ComputeDispatch dispatch{context};
    ID3D12Resource first, second;
    auto a = std::make_shared<Resource>(Resource{{&first}});
    auto b = std::make_shared<Resource>(Resource{{&second}});
    auto alias = std::make_shared<Resource>(Resource{{&first}});
    const ComputeResourceUse uses[] = {{a, 8}, {b, 128}, {a, 8}, {alias, 8}, {b, 8}};
    dispatch.UavBarrier({uses});
    assert(context.commands.calls.size() == 1);
    assert(context.commands.calls.back().size() == 1);
    assert(context.commands.calls.back()[0].UAV.pResource == nullptr);
    scoped = true;
    dispatch.UavBarrier({uses});
    assert(context.commands.calls.size() == 2);
    const auto& batch = context.commands.calls.back();
    assert(batch.size() == 2);
    assert(batch[0].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV);
    assert(batch[0].UAV.pResource == &first && batch[1].UAV.pResource == &second);
    const ComputeResourceUse reads[] = {{a, 128}, {b, 64}};
    dispatch.UavBarrier({reads});
    dispatch.UavBarrier({{}});
    assert(context.commands.calls.size() == 2 && context.profile.count == 3);
    scoped = false;
    dispatch.UavBarrier({reads});
    assert(context.commands.calls.size() == 3 && context.profile.count == 4);
}
"""
        )
        self.assertEqual(source.count("UavBarrier(bindings);"), 3)


if __name__ == "__main__":
    unittest.main()
