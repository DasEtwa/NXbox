// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/renderer_d3d12/d3d12_context.h"

namespace D3D12 {
class BufferCacheRuntime;
class Buffer : public VideoCommon::BufferBase {
public:
    Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams);
    Buffer(BufferCacheRuntime&, VAddr address, u64 size, bool sparse);
    ResourcePtr Handle() const {
        return resource;
    }
    operator ResourcePtr() const {
        return resource;
    }
    bool IsSparseCompatible() const {
        return false;
    }
    bool IsRegionUsed(u64 offset, u64 size) const {
        return usage.IsUsed(offset, size);
    }
    void MarkUsage(u64 offset, u64 size);
    void ResetUsageTracking() {
        usage.Reset();
    }
    u64 LastUsageTick() const {
        return tick;
    }

private:
    Context* context{};
    ResourcePtr resource;
    VideoCommon::UsageTracker usage{0};
    u64 tick{};
};
class BufferCacheRuntime {
public:
    explicit BufferCacheRuntime(Context& context)
        : context{context}, upload{context, D3D12_HEAP_TYPE_UPLOAD},
          readback{context, D3D12_HEAP_TYPE_READBACK} {}
    void TickFrame(Common::SlotVector<Buffer>& buffers);
    u64 CurrentTick() {
        return context.CurrentTick();
    }
    bool IsFree(u64 tick) {
        return context.IsFree(tick);
    }
    void Wait(u64 tick) {
        context.Wait(tick);
    }
    void Finish() {
        context.Finish();
    }
    u64 GetDeviceLocalMemory() const {
        return MemoryBudget::Limit;
    }
    u64 GetDeviceMemoryUsage() const {
        return context.MemoryUsage();
    }
    bool CanReportMemoryUsage() const {
        return true;
    }
    u32 GetUniformBufferAlignment() const {
        return 256;
    }
    u32 GetStorageBufferAlignment() const {
        return 16;
    }
    StagingBufferRef UploadStagingBuffer(size_t size) {
        return upload.Request(size);
    }
    StagingBufferRef DownloadStagingBuffer(size_t size, bool = false) {
        return readback.Request(size);
    }
    void FreeDeferredStagingBuffer(StagingBufferRef& ref) {
        ref = {};
    }
    bool CanReorderUpload(const Buffer&, std::span<const VideoCommon::BufferCopy>) {
        return false;
    }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}
    void CopyBuffer(ResourcePtr dst, ResourcePtr src,
                    std::span<const VideoCommon::BufferCopy> copies, bool, bool = false);
    void ClearBuffer(ResourcePtr dst, u32 offset, size_t size, u32 value);
    // P3/P5 binding hooks: no guest pipeline consumes descriptors yet.
    void BindIndexBuffer(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology,
                         Tegra::Engines::Maxwell3D::Regs::IndexFormat, u32, u32, ResourcePtr, u32,
                         u32) {}
    void BindQuadIndexBuffer(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology, u32, u32) {}
    void BindVertexBuffer(u32, ResourcePtr, u32, u32, u32) {}
    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>&) {}
    void BindTransformFeedbackBuffer(u32, ResourcePtr, u32, u32) {}
    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>&) {}
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        return upload.Request(size).mapped_span;
    }
    void BindUniformBuffer(const Buffer&, u32, u32) {}
    void BindStorageBuffer(const Buffer&, u32, u32, bool) {}
    void BindTextureBuffer(Buffer&, u32, u32, VideoCore::Surface::PixelFormat) {}
    Context& context;

private:
    StagingRing upload;
    StagingRing readback;
};
struct BufferCacheParams {
    using Runtime = BufferCacheRuntime;
    using Buffer = D3D12::Buffer;
    using Async_Buffer = StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};
using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;
} // namespace D3D12
