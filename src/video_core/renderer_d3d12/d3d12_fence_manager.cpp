// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"

#include "common/gpu_thread_profile.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/texture_cache/texture_cache.h"

namespace D3D12 {
void InnerFence::Queue() {
    if (!is_stubbed) {
        tick = !BisectEnabled("NXBOX_D3D12_FENCE_BATCHING")
                   ? context.Submit(FrameCount::ForcedSubmitGuestFence)
                   : context.PendingFenceTick();
    }
}
bool InnerFence::IsSubmitted() const {
    return is_stubbed || context.LastSubmittedTick() >= tick;
}
bool InnerFence::IsSignaled() const {
    return is_stubbed || context.IsFree(tick);
}
void InnerFence::Wait() {
    if (!is_stubbed) {
        try {
            context.WaitSubmitted(tick);
        } catch (const std::exception& error) {
            context.ReportFailure(error);
            throw;
        }
    }
}
FenceManager::FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                           TextureCache& texture_cache, BufferCache& buffer_cache,
                           QueryCache& query_cache, Context& context)
    : GenericFenceManager{rasterizer, gpu, texture_cache, buffer_cache, query_cache},
      context{context} {}
FenceManager::~FenceManager() {
    // Stop while the derived vtable and Context reference are still alive.
    StopFenceThread();
}
void FenceManager::SignalFence(std::function<void()>&& func) {
    ++pending_guest_fences;
    GenericFenceManager::SignalFence([this, func = std::move(func)] {
        func();
        --pending_guest_fences;
    });
}
void FenceManager::SyncOperation(std::function<void()> &&func) {
    ++pending_guest_callbacks;
    GenericFenceManager::SyncOperation([this, func = std::move(func)] {
        func();
        --pending_guest_callbacks;
    });
}
void FenceManager::SignalSyncPoint(u32 value) {
    syncpoint_manager.IncrementGuest(value);
    ++pending_syncpoints;
    LogSyncpoints();
    auto signal = [this, value] {
        syncpoint_manager.IncrementHost(value);
        --pending_syncpoints;
        ++signaled_syncpoints;
        LogSyncpoints();
    };
    if (!Common::GpuProfile::enabled.load(std::memory_order_relaxed)) {
        SignalFence(std::move(signal));
        return;
    }
    const auto latency =
        Common::GpuProfile::ProcessFence(value, syncpoint_manager.GetGuestSyncpointValue(value));
    SignalFence([signal = std::move(signal), latency] {
        Common::GpuProfile::CompleteFence(latency);
        signal();
    });
}
void FenceManager::SignalReference() {
    SignalFence([] {});
}
void FenceManager::LogSyncpoints() const {
    const auto signaled = signaled_syncpoints.load();
    if (signaled < 8 || signaled % 120 == 0) {
        Diagnostic("D3D12_SYNCPOINTS pending=" + std::to_string(pending_syncpoints.load()) +
                   " signaled=" + std::to_string(signaled));
    }
}
bool FenceManager::ShouldFlushFence() const {
    return !BisectEnabled("NXBOX_D3D12_FENCE_BATCHING");
}
void FenceManager::FlushPendingFences() {
    context.Submit(FrameCount::ForcedSubmitGuestFence);
}
bool FenceManager::IsFenceSubmitted(const Fence& fence) const {
    return fence->IsSubmitted();
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
