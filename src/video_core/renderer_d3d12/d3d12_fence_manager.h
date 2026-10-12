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
    // Context's allocator, event and completion batches are GPU-thread owned.
    static constexpr bool HAS_ASYNC_CHECK = false;
};
using GenericFenceManager = VideoCommon::FenceManager<FenceManagerParams>;
class FenceManager final : public GenericFenceManager {
public:
    FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                 TextureCache& texture_cache, BufferCache& buffer_cache, QueryCache& query_cache,
                 Context& context);

protected:
    Fence CreateFence(bool is_stubbed) override;
    void QueueFence(Fence& fence) override;
    bool IsFenceSignaled(Fence& fence) const override;
    void WaitFence(Fence& fence) override;

private:
    Context& context;
};
} // namespace D3D12
