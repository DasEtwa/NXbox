// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <fmt/format.h>
#include "common/alignment.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"

namespace D3D12 {
namespace {
void InvalidRange(u32 kind, const char* label, u64 offset, u64 size, u64 width) {
    static std::array<std::atomic_flag, 8> reported{};
    if (!reported[kind].test_and_set())
        Diagnostic(fmt::format(
            "D3D12 {} rejected offset={} size={} resource_bytes={}; substituting zero/skip", label,
            offset, size, width));
}
} // namespace

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
    const FrameProfile::Scope profile_copy{
        context.Profile(), src && src->heap == D3D12_HEAP_TYPE_UPLOAD ? FramePhase::BufferUpload
                           : dst && dst->heap == D3D12_HEAP_TYPE_READBACK
                               ? FramePhase::BufferDownload
                               : FramePhase::BufferCache};
    if (!dst || !src) {
        InvalidRange(7, "null buffer copy", 0, 0, 0);
        return;
    }
    const auto src_size = src->handle->GetDesc().Width;
    const auto dst_size = dst->handle->GetDesc().Width;
    for (const auto& copy : copies) {
        if (copy.src_offset > src_size || copy.size > src_size - copy.src_offset) {
            InvalidRange(0, "buffer copy source", copy.src_offset, copy.size, src_size);
            return;
        }
        if (copy.dst_offset > dst_size || copy.size > dst_size - copy.dst_offset) {
            InvalidRange(1, "buffer copy destination", copy.dst_offset, copy.size, dst_size);
            return;
        }
    }
    if (dst == src) {
        // Snapshot every source range before changing any destination range.
        size_t scratch_size{};
        for (const auto& copy : copies)
            scratch_size = CheckedAdd(scratch_size, copy.size);
        if (!scratch_size)
            return;
        auto scratch = context.CreateBuffer(scratch_size, D3D12_HEAP_TYPE_DEFAULT);
        std::vector<VideoCommon::BufferCopy> reads, writes;
        u64 cursor{};
        for (const auto& copy : copies) {
            reads.push_back({copy.src_offset, cursor, copy.size});
            writes.push_back({cursor, copy.dst_offset, copy.size});
            cursor += copy.size;
        }
        CopyBuffer(scratch, src, reads, false);
        CopyBuffer(dst, scratch, writes, false);
        return;
    }
    context.Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& copy : copies) {
        context.MarkWork("upload/copy");
        context.Commands()->CopyBufferRegion(dst->handle.Get(), copy.dst_offset, src->handle.Get(),
                                             copy.src_offset, copy.size);
    }
}
void BufferCacheRuntime::ClearBuffer(ResourcePtr dst, u32 offset, size_t size, u32 value) {
    // Copy-based fill also handles buffers without UAV flags. Chunk large clears.
    const auto width = dst ? dst->handle->GetDesc().Width : 0;
    if (!dst || ((size | offset) & 3) || offset > width || size > width - offset) {
        InvalidRange(2, "unaligned or out-of-bounds buffer clear", offset, size, width);
        return;
    }
    while (size) {
        const size_t count = std::min(size, size_t{1 << 20});
        const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
        context.Profile().Count(FrameCount::BufferUploadBytes, count);
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
    const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
    context.Profile().Count(FrameCount::BufferUploadBytes, size_t{quads} * 6 * sizeof(u32));
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
    if (index >= GeometryBindings::MaxVertexBuffers)
        return;
    const auto width = buffer ? buffer->handle->GetDesc().Width : 0;
    if (offset > width || size > width - offset) {
        InvalidRange(3, "vertex binding", offset, size, width);
        bindings.geometry.vertex[index] = {};
        return;
    }
    // Size is the inclusive Maxwell limit minus start supplied by the shared cache.
    // Preserve stride zero (constant attributes) and the synchronized guest extent.
    bindings.geometry.vertex[index] = {std::move(buffer), offset, size, stride};
}
void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& host) {
    for (size_t i = 0; i < host.buffers.size(); ++i)
        BindVertexBuffer(host.min_index + static_cast<u32>(i), host.buffers[i]->Handle(),
                         static_cast<u32>(host.offsets[i]), static_cast<u32>(host.sizes[i]),
                         static_cast<u32>(host.strides[i]));
}
std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t stage, u32, u32 size) {
    const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
    context.Profile().Count(FrameCount::BufferUploadBytes, std::max(size, u32{65536}));
    // Reserve the entire legal CBV range and initialize every byte beyond the guest upload.
    if (size > 65536) {
        InvalidRange(4, "mapped uniform exceeds Maxwell 64 KiB limit", 0, size, 65536);
        // Keep the caller's memcpy in bounds while exposing only legal bytes to D3D12.
        auto rejected = upload.Request(size);
        std::fill(rejected.mapped_span.begin(), rejected.mapped_span.end(), u8{0});
        bindings.stages[stage].uniform.push_back({nullptr, 0, 0});
        return rejected.mapped_span;
    }
    auto staging = upload.Request(65536);
    std::fill(staging.mapped_span.begin(), staging.mapped_span.end(), u8{0});
    bindings.stages[stage].uniform.push_back(
        {staging.buffer, static_cast<u32>(staging.offset), 65536});
    return staging.mapped_span.first(size);
}
void BufferCacheRuntime::BindUniformBuffer(const Buffer& buffer, u32 offset, u32 size) {
    auto resource = buffer.Handle();
    const auto width = resource ? resource->handle->GetDesc().Width : 0;
    if (!resource || !size) {
        bindings.stages[bindings.current_stage].uniform.push_back({nullptr, 0, 0});
        return;
    }
    if (size > 65536 || offset > width || size > width - offset) {
        InvalidRange(5, "uniform binding exceeds synchronized buffer range or 64 KiB", offset, size,
                     width);
        bindings.stages[bindings.current_stage].uniform.push_back({nullptr, 0, 0});
        return;
    }
    // A copied CBV owns its padding; rounding a direct view must never expose unrelated bytes.
    const auto padded = Common::AlignUp<u32>(size, 256);
    if ((offset & 255) || size != padded || padded > width - offset) {
        const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
        context.Profile().Count(FrameCount::BufferUploadBytes, 65536);
        auto staging = upload.Request(65536);
        std::fill(staging.mapped_span.begin(), staging.mapped_span.end(), u8{0});
        auto target = context.CreateBuffer(65536, D3D12_HEAP_TYPE_DEFAULT);
        const VideoCommon::BufferCopy zero{staging.offset, 0, 65536};
        CopyBuffer(target, staging.buffer, std::span{&zero, 1}, true);
        const VideoCommon::BufferCopy data{offset, 0, size};
        CopyBuffer(target, resource, std::span{&data, 1}, false);
        bindings.stages[bindings.current_stage].uniform.push_back({std::move(target), 0, 65536});
        return;
    }
    bindings.stages[bindings.current_stage].uniform.push_back({std::move(resource), offset, size});
}
void BufferCacheRuntime::BindStorageBuffer(const Buffer& buffer, u32 offset, u32 size,
                                           bool is_written) {
    auto resource = buffer.Handle();
    const auto width = resource ? resource->handle->GetDesc().Width : 0;
    if (offset > width || size > width - offset || (offset & 3)) {
        InvalidRange(6, "raw storage binding requires an in-bounds word-aligned range", offset,
                     size, width);
        bindings.stages[bindings.current_stage].storage.push_back({nullptr, 0, 0, is_written});
        return;
    }
    bindings.stages[bindings.current_stage].storage.push_back(
        {std::move(resource), offset, size, is_written});
}
void BufferCacheRuntime::BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    bindings.stages[bindings.current_stage].texel.push_back(
        {buffer.Handle(), offset, size, false, static_cast<u32>(format)});
}
} // namespace D3D12
