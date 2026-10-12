// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_formats.h"
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
        context.Finish();
    }
    StagingBufferRef UploadStagingBuffer(size_t size, bool = false) {
        return upload.Request(size);
    }
    StagingBufferRef DownloadStagingBuffer(size_t size, bool = false) {
        return readback.Request(size);
    }
    void FreeDeferredStagingBuffer(StagingBufferRef& ref) {
        ref = {};
    }
    void TickFrame();
    void FlushDeferredClear() {}
    u64 GetDeviceLocalMemory() const {
        return MemoryBudget::Limit;
    }
    u64 GetDeviceMemoryUsage() const {
        return context.MemoryUsage();
    }
    bool CanReportMemoryUsage() const {
        return true;
    }
    void ApplyMemoryBudgetOverride(u64& expected, u64& critical) {
        expected = 320ULL << 20;
        critical = 448ULL << 20;
    }
    bool CanDownloadMsaa(const VideoCommon::ImageInfo&) const {
        return false;
    }
    bool CanUploadMSAA() const {
        return false;
    }
    bool CanAccelerateImageUpload(Image&) const {
        return false;
    }
    void AccelerateImageUpload(Image&, const StagingBufferRef&,
                               std::span<const VideoCommon::SwizzleParameters>, u32, u32);
    void InsertUploadMemoryBarrier() {}
    void TransitionImageLayout(Image&);
    bool HasBrokenTextureViewFormats() const {
        return false;
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
    void ConvertImage(Framebuffer*, ImageView&, ImageView&);
    // Raw framebuffer fallback lives here alongside the shared guest image cache.
    ResourcePtr UploadFramebuffer(Tegra::MaxwellDeviceMemoryManager&,
                                  const Tegra::FramebufferConfig&);
    Context& context;

private:
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
    u64 allocation_tick{};

private:
    void Transfer(const StagingBufferRef&, std::span<const VideoCommon::BufferImageCopy>,
                  bool download);
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
    const D3D12_SHADER_RESOURCE_VIEW_DESC& Description() const {
        return description;
    }

private:
    ResourcePtr resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
};
class ImageAlloc : public VideoCommon::ImageAllocBase {};
class Sampler {
public:
    Sampler(TextureCacheRuntime&, const Tegra::Texture::TSCEntry&) {}
};
class Framebuffer {
public:
    Framebuffer(TextureCacheRuntime&, std::span<ImageView*, VideoCommon::NUM_RT>, ImageView*,
                const VideoCommon::RenderTargets&) {}
};
struct TextureCacheParams {
    static constexpr bool ENABLE_VALIDATION = true;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = true;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = false;
    static constexpr bool HAS_MSAA_DOWNLOADS = false;
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
