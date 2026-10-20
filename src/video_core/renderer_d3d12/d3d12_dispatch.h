// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RasterizerD3D12::DispatchCompute calls ComputeDispatch::Direct/Indirect after
// flushing guest memory, configuring the compute PSO and locking both caches.
// TextureCacheRuntime::AccelerateImageUpload uses ComputePasses, which shares
// this dispatch path. The caller ends attachment use and restores graphics
// bindings afterward; every dispatch changes descriptor heaps and compute state.
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace D3D12 {
inline constexpr std::uint32_t ComputeRootDwords = 16;
enum class ComputeRootParameter : std::uint32_t { Constants, Cbv, Srv, Uav, Sampler };
using ComputeConstants = std::array<std::uint32_t, ComputeRootDwords>;
using ComputeGroups = std::array<std::uint32_t, 3>;
constexpr ComputeGroups ComputeGroupCount(ComputeGroups extent, ComputeGroups local_size) {
    ComputeGroups groups{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!local_size[i])
            throw std::invalid_argument("D3D12 zero compute local size");
        groups[i] = extent[i] / local_size[i] + (extent[i] % local_size[i] != 0);
        if (groups[i] > 65535)
            throw std::out_of_range("D3D12 compute group count exceeds 65535");
    }
    return groups;
}
constexpr bool HasComputeWork(ComputeGroups groups) {
    for (auto group : groups) {
        if (group > 65535)
            throw std::out_of_range("D3D12 compute group count exceeds 65535");
    }
    return groups[0] && groups[1] && groups[2];
}
} // namespace D3D12

#ifdef _WIN32
#include "video_core/renderer_d3d12/d3d12_context.h"

namespace D3D12 {
struct ComputeBindingCounts {
    std::uint32_t cbv{}, srv{}, uav{}, sampler{};
};
// Empty ranges have one null descriptor. Space0 tables follow Shader::Info's
// order; constants are b0/space1. Internal passes keep this legacy layout.
ComPtr<ID3D12RootSignature> CreateComputeRootSignature(ID3D12Device* device,
                                                       ComputeBindingCounts counts);
struct ComputeResourceUse {
    ResourcePtr resource;
    D3D12_RESOURCE_STATES state;
};
struct ComputeBindings {
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12DescriptorHeap> resource_heap;
    ComPtr<ID3D12DescriptorHeap> sampler_heap;
    // CBV, SRV, UAV, sampler. Root-CBV layouts ignore tables[0] and bind 1..3.
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> tables{};
    bool root_cbvs{};
    std::span<const D3D12_GPU_VIRTUAL_ADDRESS> root_cbv_addresses;
    ComputeConstants constants{};
    std::span<const ComputeResourceUse> resources;
    // Texture subresources read while other subresources of the same texture are written (UAV in
    // `resources`): moved to NON_PIXEL_SHADER_RESOURCE one by one after the whole-resource batch.
    std::span<const std::pair<ResourcePtr, std::vector<std::uint32_t>>> split_reads;
};
// Heap slots must stay immutable through Context::CurrentTick(). COM objects and
// resources are retained here; the descriptor allocator owns slot retirement.
// Indirect arguments must contain legal D3D12 group counts, as on Vulkan.
class ComputeDispatch {
public:
    explicit ComputeDispatch(Context& context);
    void Direct(const ComputeBindings& bindings, ComputeGroups groups);
    void Indirect(const ComputeBindings& bindings, const ResourcePtr& arguments,
                  std::uint64_t offset);

private:
    void Bind(const ComputeBindings& bindings);
    void UavBarrier(const ComputeBindings& bindings);
    Context& context;
    ComPtr<ID3D12CommandSignature> indirect_signature;
};
} // namespace D3D12
#endif
