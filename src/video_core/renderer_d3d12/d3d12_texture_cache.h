// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_compute_pass.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_formats.h"
#include "video_core/renderer_d3d12/d3d12_texture_transfer.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/texture_cache_base.h"

namespace D3D12 {
FormatInfo MaxwellFormat(VideoCore::Surface::PixelFormat format);
class Image;
class ImageView;
class Framebuffer;
class TextureCacheRuntime {
public:
    explicit TextureCacheRuntime(Context& context)
        : context{context}, upload{context, D3D12_HEAP_TYPE_UPLOAD},
          readback{context, D3D12_HEAP_TYPE_READBACK} {}
    void Finish() {
        context.Finish(FrameCount::ForcedSubmitTextureDownload);
    }
    StagingBufferRef UploadStagingBuffer(size_t size, bool = false) {
        context.Profile().Count(FrameCount::TextureUploadBytes, size);
        return upload.Request(size);
    }
    StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false) {
        // Shared async texture retirement indexes mapped_span by absolute offset.
        // A dedicated lease starts at zero and stays alive until CPU unswizzling ends.
        if (deferred) {
            auto buffer = context.CreateBuffer(size, D3D12_HEAP_TYPE_READBACK);
            return {buffer, 0, buffer->mapped.first(size)};
        }
        return readback.Request(size);
    }
    void FreeDeferredStagingBuffer(StagingBufferRef& ref) {
        ref = {};
    }
    void TickFrame();
    void FlushDeferredClear() {}
    u64 GetDeviceLocalMemory() const {
        return context.DeviceLocalMemory();
    }
    u64 GetDeviceMemoryUsage() const {
        return context.MemoryUsage();
    }
    bool CanReportMemoryUsage() const {
        return true;
    }
#ifdef NXBOX_UWP
    void ApplyMemoryBudgetOverride(u64& expected, u64& critical) {
        expected = 320ULL << 20;
        critical = 448ULL << 20;
    }
#endif
    bool CanDownloadImage(const Image& image) const;
    bool CanDownloadMsaa(const VideoCommon::ImageInfo& info) const {
        const auto format = MaxwellFormat(info.format);
        return format.Supported() && format.planes == 1 &&
               info.type == VideoCommon::ImageType::e2D && info.resources.levels == 1 &&
               info.num_samples <= 16;
    }
    bool CanUploadMSAA() const {
        return true;
    }
    bool CanAccelerateImageUpload(Image&) const {
        return false;
    }
    void AccelerateImageUpload(Image&, const StagingBufferRef&,
                               std::span<const VideoCommon::SwizzleParameters>, u32, u32);
    void RecordSoftwareBlit(GPUVAddr address) {
        software_writes[software_write_cursor++ % software_writes.size()] = {address, frame_tick,
                                                                             true};
    }
    bool CanDiscardBlitDestination(const Image& image,
                                   const Tegra::Engines::Fermi2D::Surface& surface,
                                   const Tegra::Engines::Fermi2D::Config& copy) const;
    void RecordBlitDiscard(const Image& image);
    void RecordTextureWrite(Image& image);
    void RecordTextureUpload(Image& image);
    bool CanBlitImages(const Image& dst, const Image& src,
                       const Tegra::Engines::Fermi2D::Surface& dst_surface,
                       const Tegra::Engines::Fermi2D::Surface& src_surface,
                       const Tegra::Engines::Fermi2D::Config& copy) const;
    // Why the last CanBlitImages call refused, for the software-blit diagnostic.
    mutable const char* blit_reject = nullptr;
    void InsertUploadMemoryBarrier() {}
    void TransitionImageLayout(Image&);
    bool HasBrokenTextureViewFormats() const {
        // Maxwell view classes cross DXGI cast families. Let the shared cache
        // create format-specific aliases and synchronize their guest bit patterns.
        return true;
    }
    bool HasNativeBgr() const {
        return true;
    }
    void BarrierFeedbackLoop() {}
    void BlitImage(Framebuffer*, ImageView&, ImageView&, const VideoCommon::Region2D&,
                   const VideoCommon::Region2D&, Tegra::Engines::Fermi2D::Filter,
                   Tegra::Engines::Fermi2D::Operation);
    void CopyImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    void CopyImageMSAA(Image&, Image&, std::span<const VideoCommon::ImageCopy>);
    bool ShouldReinterpret(Image& dst, Image& src);
    void ReinterpretImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    bool ReinterpretOnGpu(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    void ConvertImage(Framebuffer*, ImageView&, ImageView&);
    // Raw framebuffer fallback lives here alongside the shared guest image cache.
    ResourcePtr UploadFramebuffer(Tegra::MaxwellDeviceMemoryManager&,
                                  const Tegra::FramebufferConfig&);
    ComputePasses& ComputeHelpers();
    BlitImageHelper& BlitHelpers();
    ResourcePtr ResolveFramebuffer(ImageView& view);
    Context& context;

private:
    std::unique_ptr<ComputePasses> compute_passes;
    struct SoftwareWrite {
        GPUVAddr address{};
        u64 frame{};
        bool valid{};
    };
    std::array<SoftwareWrite, 64> software_writes{};
    size_t software_write_cursor{};
    RepeatedTextureUploads repeated_uploads;
    std::chrono::steady_clock::time_point upload_log_time{std::chrono::steady_clock::now()};
    std::unique_ptr<BlitImageHelper> blit_helper;
    StagingRing upload;
    StagingRing readback;
    struct FramebufferEntry {
        DAddr address;
        ResourcePtr resource;
        u64 last_frame;
    };
    std::vector<FramebufferEntry> framebuffers;
    u64 frame_tick{};
    std::vector<u8> framebuffer_tiled;
    std::vector<u8> framebuffer_linear;
};
class Image : public VideoCommon::ImageBase {
public:
    Image(TextureCacheRuntime&, const VideoCommon::ImageInfo&, GPUVAddr, VAddr);
    explicit Image(const VideoCommon::NullImageParams& params) : ImageBase{params} {}
    void UploadMemory(const StagingBufferRef&, std::span<const VideoCommon::BufferImageCopy>);
    void DownloadMemory(const StagingBufferRef&, std::span<const VideoCommon::BufferImageCopy>);
    void UploadMemory(ResourcePtr, size_t, std::span<const VideoCommon::BufferImageCopy>);
    void DownloadMemory(ResourcePtr, size_t, std::span<const VideoCommon::BufferImageCopy>);
    void DownloadMemory(std::span<const ResourcePtr> buffers, std::span<const size_t> offsets,
                        std::span<const VideoCommon::BufferImageCopy> copies);
    bool IsRescaled() const {
        return false;
    }
    bool ScaleUp(bool = false) {
        return false;
    }
    bool ScaleDown(bool = false) {
        return false;
    }
    ResourcePtr Handle() const {
        return resource;
    }
    FormatInfo HostFormat() const {
        return host_format;
    }
    std::string_view upload_reason{"initial_or_recreated"};
    u64 allocation_tick{};

private:
    bool TransferConverted(const StagingBufferRef&, std::span<const VideoCommon::BufferImageCopy>,
                           bool download);
    void Transfer(const StagingBufferRef&, std::span<const VideoCommon::BufferImageCopy>,
                  bool download, const ResourcePtr& gpu_destination = {}, size_t gpu_offset = 0);
    TextureCacheRuntime* runtime{};
    ResourcePtr resource;
    FormatInfo host_format{};
};
class ImageView : public VideoCommon::ImageViewBase {
public:
    ImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo&, VideoCommon::ImageId,
              Image&);
    ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info,
              VideoCommon::ImageId id, Image& image, const Common::SlotVector<Image>&)
        : ImageView{runtime, info, id, image} {}
    ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo&,
              const VideoCommon::ImageViewInfo&, GPUVAddr);
    ImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams& params);
    bool IsRescaled() const {
        return false;
    }
    ResourcePtr ImageHandle() const {
        return resource;
    }
    // Borrow only while the view is alive; callers retaining work still copy the lease.
    const ResourcePtr& BorrowImageHandle() const& {
        return resource;
    }
    FormatInfo HostFormat() const {
        auto result = host_format;
        if (resource && !host_format.depth)
            result.view_format = static_cast<u32>(description.Format);
        return result;
    }
    void LogUnsupported(std::string_view condition) const;
    const D3D12_SHADER_RESOURCE_VIEW_DESC& Description() const {
        return description;
    }
    // P3: texel-buffer views are bound by the buffer cache from these.
    GPUVAddr GpuAddr() const noexcept {
        return gpu_addr;
    }
    u32 BufferSize() const noexcept {
        return buffer_size;
    }

private:
    u32 buffer_size{};
    u32 sample_count{1};
    VideoCommon::ImageType image_type{VideoCommon::ImageType::e2D};
    FormatInfo host_format{};
    ResourcePtr resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
};
class ImageAlloc : public VideoCommon::ImageAllocBase {};
class Sampler {
public:
    Sampler(TextureCacheRuntime&, const Tegra::Texture::TSCEntry& entry) : entry{entry} {}
    // P3: descriptors are built from the guest sampler (see MakeSamplerDesc).
    const Tegra::Texture::TSCEntry& Entry() const {
        return entry;
    }

private:
    Tegra::Texture::TSCEntry entry;
};
// P3: records the attachment views; RTV/DSV descriptors are created per bind by the
// descriptor allocator. Output slot i writes to Color(draw_buffers[i]).
class Framebuffer {
public:
    Framebuffer(TextureCacheRuntime&, std::span<ImageView*, VideoCommon::NUM_RT> color,
                ImageView* depth, const VideoCommon::RenderTargets& key)
        : draw_buffers{key.draw_buffers}, size{key.size} {
        // SlotVector relocates views as descriptors are discovered. Own snapshots so the
        // format key and RTV/DSV bindings keep referencing the same live resources.
        for (size_t i = 0; i < colors.size(); ++i)
            if (color[i])
                colors[i] = *color[i];
        if (depth)
            this->depth = *depth;
    }
    ImageView* Color(size_t rt_index) {
        return colors[rt_index] ? &*colors[rt_index] : nullptr;
    }
    ImageView* Depth() {
        return depth ? &*depth : nullptr;
    }
    u8 DrawBuffer(size_t slot) const {
        return draw_buffers[slot];
    }
    VideoCommon::Extent2D Size() const {
        return size;
    }

private:
    std::array<std::optional<ImageView>, VideoCommon::NUM_RT> colors{};
    std::optional<ImageView> depth;
    std::array<u8, VideoCommon::NUM_RT> draw_buffers{};
    VideoCommon::Extent2D size{};
};
struct TextureCacheParams {
    static constexpr bool ENABLE_VALIDATION = true;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = true;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = true;
    static constexpr bool HAS_MSAA_DOWNLOADS = true;
    using Runtime = TextureCacheRuntime;
    using Image = D3D12::Image;
    using ImageAlloc = D3D12::ImageAlloc;
    using ImageView = D3D12::ImageView;
    using Sampler = D3D12::Sampler;
    using Framebuffer = D3D12::Framebuffer;
    using AsyncBuffer = StagingBufferRef;
    using BufferType = ResourcePtr;
};
using TextureCache = VideoCommon::TextureCache<TextureCacheParams>;
} // namespace D3D12
