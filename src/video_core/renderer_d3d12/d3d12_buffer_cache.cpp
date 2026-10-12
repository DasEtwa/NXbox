// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "common/alignment.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"

namespace D3D12 {
Buffer::Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams params) : BufferBase{params} {}
Buffer::Buffer(BufferCacheRuntime& runtime, VAddr address, u64 size, bool)
    : BufferBase{address, size}, context{&runtime.context},
      resource{context->CreateBuffer(size, D3D12_HEAP_TYPE_DEFAULT)}, usage{size} {}
void Buffer::MarkUsage(u64 offset, u64 size) {
    if (!context || !resource)
        return;
    usage.Track(offset, size);
    tick = context->CurrentTick();
    context->Retain(resource);
}
void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& buffers) {
    upload.Collect();
    readback.Collect();
    for (const auto [id, buffer] : buffers)
        buffer->ResetUsageTracking();
}
void BufferCacheRuntime::CopyBuffer(ResourcePtr dst, ResourcePtr src,
                                    std::span<const VideoCommon::BufferCopy> copies, bool, bool) {
    if (dst == src)
        throw std::runtime_error("D3D12 overlapping buffer copy requires scratch");
    context.Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto src_size = src->handle->GetDesc().Width;
    const auto dst_size = dst->handle->GetDesc().Width;
    for (const auto& copy : copies) {
        if (copy.src_offset > src_size || copy.size > src_size - copy.src_offset ||
            copy.dst_offset > dst_size || copy.size > dst_size - copy.dst_offset)
            throw std::out_of_range("D3D12 buffer copy bounds");
        context.MarkWork("upload/copy");
        context.Commands()->CopyBufferRegion(dst->handle.Get(), copy.dst_offset, src->handle.Get(),
                                             copy.src_offset, copy.size);
    }
}
void BufferCacheRuntime::ClearBuffer(ResourcePtr dst, u32 offset, size_t size, u32 value) {
    // Copy-based fill also handles buffers without UAV flags. Chunk large clears.
    if ((size | offset) & 3)
        throw std::invalid_argument("D3D12 unaligned buffer clear");
    while (size) {
        const size_t count = std::min(size, size_t{1 << 20});
        auto staging = upload.Request(count);
        for (size_t i = 0; i < count; i += sizeof(value))
            std::memcpy(staging.mapped_span.data() + i, &value, sizeof(value));
        const VideoCommon::BufferCopy copy{staging.offset, offset, count};
        CopyBuffer(dst, staging.buffer, std::span{&copy, 1}, true);
        offset += static_cast<u32>(count);
        size -= count;
        // Prevent a single large clear from exhausting the staging ring.
        if (size)
            context.Finish();
    }
}
using Maxwell = Tegra::Engines::Maxwell3D::Regs;
void BufferCacheRuntime::BindIndexBuffer(Maxwell::PrimitiveTopology topology,
                                         Maxwell::IndexFormat format, u32, u32, ResourcePtr buffer,
                                         u32 offset, u32 size) {
    auto& geometry = bindings.geometry;
    geometry.index = std::move(buffer);
    geometry.index_offset = offset;
    geometry.index_size = size;
    geometry.index_is_32bit = format == Maxwell::IndexFormat::UnsignedInt;
    // 8-bit indices and indexed quads need a conversion pass that is not implemented yet.
    geometry.index_unsupported = format == Maxwell::IndexFormat::UnsignedByte ||
                                 topology == Maxwell::PrimitiveTopology::Quads ||
                                 topology == Maxwell::PrimitiveTopology::QuadStrip;
}
void BufferCacheRuntime::BindQuadIndexBuffer(Maxwell::PrimitiveTopology topology, u32 first,
                                             u32 count) {
    // Quads are drawn as indexed triangle lists; indices already include the first vertex.
    static constexpr u32 quad_list[6] = {0, 1, 2, 0, 2, 3};
    static constexpr u32 strip_list[6] = {0, 1, 2, 2, 1, 3};
    const bool strip = topology == Maxwell::PrimitiveTopology::QuadStrip;
    const u32 quads = strip ? (count < 4 ? 0 : (count - 2) / 2) : count / 4;
    auto& geometry = bindings.geometry;
    geometry.index_unsupported = false;
    geometry.index_is_32bit = true;
    if (!quads) {
        geometry.index = nullptr;
        geometry.index_size = 0;
        return;
    }
    auto staging = upload.Request(size_t{quads} * 6 * sizeof(u32));
    auto* out = reinterpret_cast<u32*>(staging.mapped_span.data());
    for (u32 quad = 0; quad < quads; ++quad) {
        const u32 base = first + quad * (strip ? 2 : 4);
        for (u32 i = 0; i < 6; ++i)
            *out++ = base + (strip ? strip_list[i] : quad_list[i]);
    }
    geometry.index = staging.buffer;
    geometry.index_offset = staging.offset;
    geometry.index_size = staging.mapped_span.size();
}
void BufferCacheRuntime::BindVertexBuffer(u32 index, ResourcePtr buffer, u32 offset, u32 size,
                                          u32 stride) {
    if (index < GeometryBindings::MaxVertexBuffers)
        bindings.geometry.vertex[index] = {std::move(buffer), offset, size, stride};
}
void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& host) {
    for (size_t i = 0; i < host.buffers.size(); ++i)
        BindVertexBuffer(host.min_index + static_cast<u32>(i), host.buffers[i]->Handle(),
                         static_cast<u32>(host.offsets[i]), static_cast<u32>(host.sizes[i]),
                         static_cast<u32>(host.strides[i]));
}
std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t stage, u32, u32 size) {
    // CBV views are 256-byte multiples: reserve the rounded size, expose the requested one.
    auto staging = upload.Request(Common::AlignUp<size_t>(size, 256));
    bindings.stages[stage].uniform.push_back(
        {staging.buffer, static_cast<u32>(staging.offset), size});
    return staging.mapped_span.first(size);
}
void BufferCacheRuntime::BindUniformBuffer(const Buffer& buffer, u32 offset, u32 size) {
    bindings.stages[bindings.current_stage].uniform.push_back({buffer.Handle(), offset, size});
}
void BufferCacheRuntime::BindStorageBuffer(const Buffer& buffer, u32 offset, u32 size,
                                           bool is_written) {
    bindings.stages[bindings.current_stage].storage.push_back(
        {buffer.Handle(), offset, size, is_written});
}
void BufferCacheRuntime::BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    bindings.stages[bindings.current_stage].texel.push_back(
        {buffer.Handle(), offset, size, false, static_cast<u32>(format)});
}
} // namespace D3D12
