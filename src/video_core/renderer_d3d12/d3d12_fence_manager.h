// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/fence_manager.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"

namespace D3D12 {
class InnerFence final : public VideoCommon::FenceBase {
public:
    InnerFence(Context& context, bool is_stubbed) : FenceBase{is_stubbed}, context{context} {}
    void Queue();
    bool IsSignaled() const;
    bool IsSubmitted() const;
    void Wait();

private:
    Context& context;
    u64 tick{};
};
using Fence = std::shared_ptr<InnerFence>;
struct FenceManagerParams {
    using FenceType = Fence;
    using BufferCacheType = BufferCache;
    using TextureCacheType = TextureCache;
    using QueryCacheType = QueryCache;
    // The completion thread waits for captured ticks; only the recording thread submits.
    static constexpr bool HAS_ASYNC_CHECK = true;
    static constexpr bool REQUIRE_GPU_COMPLETION = true;
};
using GenericFenceManager = VideoCommon::FenceManager<FenceManagerParams>;
class FenceManager final : public GenericFenceManager {
public:
    FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                 TextureCache& texture_cache, BufferCache& buffer_cache, QueryCache& query_cache,
                 Context& context);

    ~FenceManager() override;
    void SignalFence(std::function<void()>&& func);
    void SignalSyncPoint(u32 value);
    void SyncOperation(std::function<void()> &&func);
    u64 PendingGuestFences() const { return pending_guest_fences.load(); }
    u64 PendingGuestCallbacks() const { return pending_guest_callbacks.load(); }
    void SignalReference();
    void LogSyncpoints() const;
    // Recording thread, batching only: submit the pending tick before blocking on a fence.
    void FlushPendingFences() override;
    // Recording thread, batching only: wake the fencing thread after a submission made elsewhere.
    void NotifyDeferredSubmit();

protected:
    Fence CreateFence(bool is_stubbed) override;
    void QueueFence(Fence& fence) override;
    bool IsFenceSignaled(Fence& fence) const override;
    void WaitFence(Fence& fence) override;
    bool ShouldFlushFence() const override;
    bool IsFenceSubmitted(const Fence& fence) const override;

private:
    Context& context;
    std::atomic<u64> pending_guest_fences{}, pending_guest_callbacks{};
    std::atomic<u64> pending_syncpoints{};
    std::atomic<u64> signaled_syncpoints{};
};
} // namespace D3D12
