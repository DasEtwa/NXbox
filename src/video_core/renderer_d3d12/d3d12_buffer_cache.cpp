// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <stdexcept>
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
} // namespace D3D12
