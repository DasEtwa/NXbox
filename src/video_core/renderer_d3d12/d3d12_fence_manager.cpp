// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"

#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/texture_cache/texture_cache.h"

namespace D3D12 {
void InnerFence::Queue() {
    if (!is_stubbed) {
        tick = context.Submit();
    }
}
bool InnerFence::IsSignaled() const {
    return is_stubbed || context.IsFree(tick);
}
void InnerFence::Wait() {
    if (!is_stubbed) {
        context.Wait(tick);
    }
}
FenceManager::FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                           TextureCache& texture_cache, BufferCache& buffer_cache,
                           QueryCache& query_cache, Context& context)
    : GenericFenceManager{rasterizer, gpu, texture_cache, buffer_cache, query_cache},
      context{context} {}
void FenceManager::SignalFence(std::function<void()>&& func) {
    GenericFenceManager::SignalFence(std::move(func));
    // Context is GPU-thread owned. Drain here so guest waits cannot strand the last
    // callback when no further GPU command or presentation arrives to poll it.
    WaitPendingFences(true);
}
void FenceManager::SignalSyncPoint(u32 value) {
    syncpoint_manager.IncrementGuest(value);
    ++pending_syncpoints;
    LogSyncpoints();
    SignalFence([this, value] {
        syncpoint_manager.IncrementHost(value);
        --pending_syncpoints;
        ++signaled_syncpoints;
        LogSyncpoints();
    });
}
void FenceManager::SignalReference() {
    SignalFence([] {});
}
void FenceManager::LogSyncpoints() const {
    if (signaled_syncpoints < 8 || signaled_syncpoints % 120 == 0) {
        Diagnostic("D3D12_SYNCPOINTS pending=" + std::to_string(pending_syncpoints) +
                   " signaled=" + std::to_string(signaled_syncpoints));
    }
}
Fence FenceManager::CreateFence(bool is_stubbed) {
    return std::make_shared<InnerFence>(context, is_stubbed);
}
void FenceManager::QueueFence(Fence& fence) {
    fence->Queue();
}
bool FenceManager::IsFenceSignaled(Fence& fence) const {
    return fence->IsSignaled();
}
void FenceManager::WaitFence(Fence& fence) {
    fence->Wait();
}
} // namespace D3D12
