// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// TextureCacheRuntime::AccelerateImageUpload calls DecodeAstc/Unswizzle via Run;
// Image::UploadMemory supplies a per-mip array/3D UAV and swizzle parameters.
// BufferCacheRuntime's uint8/quad index hooks call Run before index-buffer use.
// QueryCacheRuntime's prefix accumulation hook dispatches ordered QueriesPrefix
// chunks. GPU ASTC writes RGBA8 through an UNORM UAV (also for sRGB storage);
// CPU BC1/BC3 recompression is outside this helper. Construct ComputePasses at renderer startup
// (DXC compilation is eager). These hooks are integration points; no cache/rasterizer file is
// changed here.
#include "video_core/renderer_d3d12/d3d12_dispatch.h"

namespace D3D12 {
enum class HelperComputePass : std::size_t {
    DecodeAstc,
    Unswizzle2D,
    Unswizzle3D,
    Unswizzle3DBcn,
    QueriesPrefix,
    Uint8ToUint16,
    QuadIndexed,
    Count
};
// HLSL constants use individual DWORD slots; no C++/HLSL struct packing dependency.
struct AstcDecodeParameters {
    std::uint32_t block_width{}, block_height{}, layer_stride{}, block_size{};
    std::uint32_t x_shift{}, gob_height{}, gob_height_mask{};
    constexpr ComputeConstants Constants() const {
        return {block_width, block_height, layer_stride,   block_size,
                x_shift,     gob_height,   gob_height_mask};
    }
};
struct UnswizzleParameters {
    ComputeGroups origin{};
    std::uint32_t bytes_per_block_log2{};
    std::array<std::int32_t, 3> destination{};
    std::uint32_t stride{}, block_size{}, x_shift{}, block_height{}, block_height_mask{};
    std::uint32_t block_depth{}, block_depth_mask{};
    constexpr ComputeConstants Constants() const {
        return {origin[0],
                origin[1],
                origin[2],
                bytes_per_block_log2,
                static_cast<std::uint32_t>(destination[0]),
                static_cast<std::uint32_t>(destination[1]),
                static_cast<std::uint32_t>(destination[2]),
                stride,
                block_size,
                x_shift,
                block_height,
                block_height_mask,
                block_depth,
                block_depth_mask};
    }
};
// BCn output uses 256-byte rows and a 512-byte UAV base for CopyTextureRegion;
// origin.xy is in texels, origin.z in slices. Run fills output pitches in slots 14/15.
// Constants: blocks_dim.xyz, bytes_per_block_log2, origin.xyz, slice_size,
// block_size, x_shift, block_height, block_height_mask, block_depth, block_depth_mask.
// Queries: min_accumulation_base, max_accumulation_base (local reset index + 1),
// accumulation_limit (count - 1), buffer_offset. A batch contains 1..2048 queries.
// Uint8: index_count. Quad: primitive_count, base_vertex, index_shift, is_strip.
constexpr ComputeGroups HelperGroupCount(HelperComputePass pass, ComputeGroups extent) {
    switch (pass) {
    case HelperComputePass::DecodeAstc:
        return ComputeGroupCount(extent, {8, 8, 1}); // Extent is ASTC blocks/layers.
    case HelperComputePass::Unswizzle2D:
    case HelperComputePass::Unswizzle3D:
        return ComputeGroupCount(extent, {8, 8, 1});
    case HelperComputePass::Unswizzle3DBcn:
        return ComputeGroupCount(extent, {8, 8, 4});
    case HelperComputePass::Uint8ToUint16:
        return ComputeGroupCount({extent[0] / 2 + extent[0] % 2, 1, 1}, {256, 1, 1});
    case HelperComputePass::QuadIndexed:
        return ComputeGroupCount({extent[0], 1, 1}, {256, 1, 1});
    case HelperComputePass::QueriesPrefix:
        if (extent[0] > 2048)
            throw std::out_of_range("D3D12 prefix batch exceeds 2048 queries");
        return {extent[0] ? 1u : 0u, 1, 1};
    default:
        throw std::invalid_argument("D3D12 unknown helper compute pass");
    }
}
} // namespace D3D12

#ifdef _WIN32
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
namespace D3D12 {
struct ComputeBufferView {
    ResourcePtr resource;
    std::uint64_t offset{}, size{}; // Raw UAV ranges must be DWORD aligned/padded.
};
struct ComputeTextureView {
    ResourcePtr resource;
    D3D12_UNORDERED_ACCESS_VIEW_DESC descriptor{}; // Explicit typed per-mip UAV.
};
class ComputePasses {
public:
    explicit ComputePasses(Context& context);
    // Copies an UPLOAD/COPY_SOURCE range to a DEFAULT buffer with ALLOW_UNORDERED_ACCESS.
    // Context::CreateBuffer deliberately does not set that flag; don't bind it as UAV.
    ResourcePtr UploadInput(const ResourcePtr& staging, std::uint64_t offset, std::uint64_t size);
    // extent is output texels/blocks/layers, index/primitive count, or query count.
    // buffers are u0 input, u1 output, u2 accumulator; texture output occupies u1
    // instead of the output buffer. Unswizzle requires a UINT-compatible UAV;
    // ASTC requires RGBA8 UNORM. All resources must have ALLOW_UNORDERED_ACCESS.
    // Run reuses immutable tables in shared fence-retired descriptor heaps. It fills the last two
    // root slots with the unswizzle XY extent to clip partial workgroups.
    void Run(HelperComputePass pass, ComputeConstants constants, ComputeGroups extent,
             std::span<const ComputeBufferView> buffers, ComputeTextureView texture = {});

private:
    Context& context;
    ComputeDispatch dispatch;
    D3D12DescriptorAllocator descriptors;
    ComPtr<ID3D12RootSignature> root_signature;
    std::array<ComPtr<ID3D12PipelineState>, static_cast<std::size_t>(HelperComputePass::Count)>
        pipelines;
};
} // namespace D3D12
#endif
