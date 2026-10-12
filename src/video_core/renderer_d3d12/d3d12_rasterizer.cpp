// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/control/channel_state.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

namespace D3D12 {

AccelerateDMA::AccelerateDMA() = default;

bool AccelerateDMA::BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) {
    return false;
}
bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    return false;
}

RasterizerD3D12::RasterizerD3D12(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& memory,
                                 Context& context)
    : context{context}, device_memory{memory}, buffer_runtime{context}, texture_runtime{context},
      buffer_cache{memory, buffer_runtime}, texture_cache{texture_runtime, memory}, m_gpu{gpu} {}
RasterizerD3D12::~RasterizerD3D12() = default;

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) {
    if (!draw_logged) {
        LOG_WARNING(Render, "D3D12 P2: guest graphics pipelines are stubbed (TODO P3)");
        draw_logged = true;
    }
}
void RasterizerD3D12::DrawTexture() {}
void RasterizerD3D12::Clear(u32 layer_count) {}
void RasterizerD3D12::DispatchCompute() {
    if (!compute_logged) {
        LOG_WARNING(Render, "D3D12 P2: guest compute is stubbed (TODO P4)");
        compute_logged = true;
    }
}
void RasterizerD3D12::ResetCounter(VideoCommon::QueryType type) {}
void RasterizerD3D12::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    // TODO(P5): resolve actual GPU query values; P1 writes only the guest payload/timestamp.
    if (!gpu_memory) {
        return;
    }
    if (True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout)) {
        u64 ticks = m_gpu.GetTicks();
        gpu_memory->Write<u64>(gpu_addr + 8, ticks);
        gpu_memory->Write<u64>(gpu_addr, static_cast<u64>(payload));
    } else {
        gpu_memory->Write<u32>(gpu_addr, payload);
    }
}
void RasterizerD3D12::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                u32 size) {}
void RasterizerD3D12::DisableGraphicsUniformBuffer(size_t stage, u32 index) {}
void RasterizerD3D12::FlushAll() {
    FlushCommands();
}
void RasterizerD3D12::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (!addr || !size)
        return;
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    if (True(which & VideoCommon::CacheType::BufferCache))
        buffer_cache.DownloadMemory(addr, size);
    if (True(which & VideoCommon::CacheType::TextureCache))
        texture_cache.DownloadMemory(addr, size);
}
bool RasterizerD3D12::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    return (True(which & VideoCommon::CacheType::BufferCache) &&
            buffer_cache.IsRegionGpuModified(addr, size)) ||
           (True(which & VideoCommon::CacheType::TextureCache) &&
            texture_cache.IsRegionGpuModified(addr, size));
}
void RasterizerD3D12::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (!addr || !size)
        return;
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    if (True(which & VideoCommon::CacheType::BufferCache))
        buffer_cache.WriteMemory(addr, size);
    if (True(which & VideoCommon::CacheType::TextureCache))
        texture_cache.WriteMemory(addr, size);
}
bool RasterizerD3D12::OnCPUWrite(DAddr addr, u64 size) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const bool cached = buffer_cache.OnCPUWrite(addr, size);
    texture_cache.WriteMemory(addr, size);
    return cached;
}
void RasterizerD3D12::OnCacheInvalidation(DAddr addr, u64 size) {
    InvalidateRegion(addr, size);
}
VideoCore::RasterizerDownloadArea RasterizerD3D12::GetFlushArea(PAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        if (const auto area = texture_cache.GetFlushArea(addr, size))
            return *area;
    }
    VideoCore::RasterizerDownloadArea new_area{
        .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}
void RasterizerD3D12::InvalidateGPUCache() {
    m_gpu.InvalidateGPUCache();
}
void RasterizerD3D12::UnmapMemory(DAddr addr, u64 size) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    texture_cache.UnmapMemory(addr, size);
    buffer_cache.WriteMemory(addr, size);
}
void RasterizerD3D12::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    texture_cache.UnmapGPUMemory(as_id, addr, size);
    buffer_cache.UnmapGPUMemory(as_id, addr, size);
}
void RasterizerD3D12::SignalFence(std::function<void()>&& func) {
    FlushCommands();
    func();
}
void RasterizerD3D12::SyncOperation(std::function<void()>&& func) {
    FlushCommands();
    func();
}
void RasterizerD3D12::SignalSyncPoint(u32 value) {
    FlushCommands();
    auto& syncpoint_manager = m_gpu.Host1x().GetSyncpointManager();
    syncpoint_manager.IncrementGuest(value);
    syncpoint_manager.IncrementHost(value);
}
void RasterizerD3D12::SignalReference() {}
void RasterizerD3D12::ReleaseFences(bool force) {
    if (force)
        FlushCommands();
}
void RasterizerD3D12::FlushAndInvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    FlushRegion(addr, size, which);
    InvalidateRegion(addr, size, which);
}
void RasterizerD3D12::WaitForIdle() {
    FlushCommands();
}
void RasterizerD3D12::FragmentBarrier() {}
void RasterizerD3D12::TiledCacheBarrier() {}
void RasterizerD3D12::FlushCommands() {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.AccumulateFlushes();
    buffer_cache.CommitAsyncFlushes();
    texture_cache.CommitAsyncFlushes();
    context.Finish();
    buffer_cache.PopAsyncFlushes();
    texture_cache.PopAsyncFlushes();
}
void RasterizerD3D12::TickFrame() {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.TickFrame();
    texture_cache.TickFrame();
}
Tegra::Engines::AccelerateDMAInterface& RasterizerD3D12::AccessAccelerateDMA() {
    return m_accelerate_dma;
}
bool RasterizerD3D12::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& copy_config) {
    return false;
}
void RasterizerD3D12::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                               std::span<const u8> memory) {
    if (gpu_memory && copy_size <= memory.size()) {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
    }
}
void RasterizerD3D12::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                        const VideoCore::DiskResourceLoadCallback& callback) {}
void RasterizerD3D12::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.CreateChannel(channel);
    texture_cache.CreateChannel(channel);
}
void RasterizerD3D12::BindChannel(Tegra::Control::ChannelState& channel) {
    BindToChannel(channel.bind_id);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.BindToChannel(channel.bind_id);
    texture_cache.BindToChannel(channel.bind_id);
}
void RasterizerD3D12::ReleaseChannel(s32 channel_id) {
    EraseChannel(channel_id);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.EraseChannel(channel_id);
    texture_cache.EraseChannel(channel_id);
}

RasterizerD3D12::DisplayTexture RasterizerD3D12::FramebufferTexture(
    const Tegra::FramebufferConfig& config) {
    std::scoped_lock lock{texture_cache.mutex};
    const auto [view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, config.address + config.offset);
    if (view && view->ImageHandle()) {
        texture_cache.PrepareImage(view->image_id, false, false);
        auto description = view->Description();
        if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            const auto mip = description.Texture2D.MostDetailedMip;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray = {mip, 1, 0, 1, 0, 0};
        } else if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
            description.Texture2DArray.ArraySize = 1;
            description.Texture2DArray.MipLevels = 1;
        } else {
            throw std::runtime_error("D3D12 framebuffer must be a 2D texture view");
        }
        return {view->ImageHandle(), description};
    }
    auto resource = texture_runtime.UploadFramebuffer(device_memory, config);
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = static_cast<DXGI_FORMAT>(
        MaxwellFormat(VideoCore::Surface::PixelFormatFromGPUPixelFormat(config.pixel_format))
            .view_format);
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2DArray = {0, 1, 0, 1, 0, 0};
    return {resource, description};
}
} // namespace D3D12
