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
