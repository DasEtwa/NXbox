// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "common/settings.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/texture_cache/image_view_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

namespace D3D12 {
FormatInfo MaxwellFormat(VideoCore::Surface::PixelFormat format) {
    switch (format) {
#define PIXEL_FORMAT_ELEM(name, ...)                                                               \
    case VideoCore::Surface::PixelFormat::name:                                                    \
        return LookupFormat(#name);
        PIXEL_FORMAT_LIST
#undef PIXEL_FORMAT_ELEM
    default:
        return LookupFormat("");
    }
}
namespace {
ResourcePtr MakeTexture(Context& context, const VideoCommon::ImageInfo& info, FormatInfo format) {
    if (!format.Supported() || format.planes != 1 || info.num_samples != 1 ||
        info.type == VideoCommon::ImageType::Buffer)
        throw std::runtime_error("D3D12 P2 unsupported texture format/planes/samples/type");
    if (!info.size.width || !info.size.height || !info.size.depth || info.resources.levels <= 0 ||
        info.resources.levels > 16 || info.resources.layers <= 0 || info.resources.layers > 2048 ||
        info.size.depth > 2048)
        throw std::invalid_argument("D3D12 invalid image dimensions");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = info.type == VideoCommon::ImageType::e1D ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                     : info.type == VideoCommon::ImageType::e3D
                         ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                         : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width =
        (u64{info.size.width} + format.block_width - 1) / format.block_width * format.block_width;
    desc.Height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                      ? 1
                      : (info.size.height + format.block_height - 1) / format.block_height *
                            format.block_height;
    desc.DepthOrArraySize = static_cast<UINT16>(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                    ? info.size.depth
                                                    : info.resources.layers);
    desc.MipLevels = static_cast<UINT16>(info.resources.levels);
    desc.Format = static_cast<DXGI_FORMAT>(format.resource_format);
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{static_cast<DXGI_FORMAT>(format.view_format)};
    Check(context.Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                sizeof(support)),
          "texture format support", context.Device());
    const auto required =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D   ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
        : desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                                                               : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
    if (!(support.Support1 & required))
        throw std::runtime_error("D3D12 device does not support texture format");
    // P3: guest draws render into cached images.
    if (format.depth) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT depth_support{
            DsvFormat(static_cast<DXGI_FORMAT>(format.view_format))};
        Check(context.Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &depth_support,
                                                    sizeof(depth_support)),
              "depth format support", context.Device());
        if (depth_support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    } else if ((support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) &&
               desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (!format.depth && format.block_width == 1 && format.block_height == 1) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT uav_support{
            format.converted_astc ? DXGI_FORMAT_R8G8B8A8_UNORM
                                  : static_cast<DXGI_FORMAT>(format.view_format)};
        Check(context.Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &uav_support,
                                                    sizeof(uav_support)),
              "texture UAV support", context.Device());
        if (uav_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    return context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
}
UINT Subresource(const VideoCommon::ImageInfo& info, s32 level, s32 layer) {
    if (level < 0 || level >= info.resources.levels || layer < 0 || layer >= info.resources.layers)
        throw std::out_of_range("D3D12 image subresource");
    return static_cast<UINT>(level + layer * info.resources.levels);
}
} // namespace
Image::Image(TextureCacheRuntime& runtime, const VideoCommon::ImageInfo& info, GPUVAddr gpu,
             VAddr cpu)
    : ImageBase{info, gpu, cpu}, allocation_tick{runtime.context.CurrentTick()}, runtime{&runtime},
      host_format{MaxwellFormat(info.format)} {
    if (host_format.converted_astc) {
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
        switch (Settings::values.astc_recompression.GetValue()) {
        case Settings::AstcRecompression::Bc1:
            host_format = LookupFormat("BC1_RGBA_UNORM");
            break;
        case Settings::AstcRecompression::Bc3:
            host_format = LookupFormat("BC3_UNORM");
            break;
        default:
            break;
        }
        if (VideoCore::Surface::IsPixelFormatSRGB(info.format)) {
            if (host_format.block_width == 4)
                host_format = LookupFormat(Settings::values.astc_recompression.GetValue() ==
                                                   Settings::AstcRecompression::Bc1
                                               ? "BC1_RGBA_SRGB"
                                               : "BC3_SRGB");
        }
    }
    resource = MakeTexture(runtime.context, info, host_format);
    if (!BisectEnabled("NXBOX_D3D12_NO_GPU_ASTC") &&
        VideoCore::Surface::IsPixelFormatASTC(info.format) &&
        info.type == VideoCommon::ImageType::e2D && info.num_samples == 1 &&
        host_format.block_width == 1 &&
        (resource->handle->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
    }
}
void Image::UploadMemory(const StagingBufferRef& map,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    Transfer(map, copies, false);
}
void Image::DownloadMemory(const StagingBufferRef& map,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    if (host_format.converted_astc || VideoCore::Surface::IsPixelFormatASTC(info.format))
        throw std::runtime_error("D3D12 converted ASTC readback is not reversible");
    Transfer(map, copies, true);
}
void Image::UploadMemory(ResourcePtr buffer, size_t offset,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    if (!buffer || offset > buffer->mapped.size())
        throw std::runtime_error("D3D12 P2 upload requires mapped staging");
    UploadMemory({buffer, offset, buffer->mapped.subspan(offset)}, copies);
}
void Image::DownloadMemory(ResourcePtr buffer, size_t offset,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    if (!buffer || offset > buffer->mapped.size())
        throw std::runtime_error("D3D12 P2 download requires mapped staging");
    DownloadMemory({buffer, offset, buffer->mapped.subspan(offset)}, copies);
}
void Image::Transfer(const StagingBufferRef& input,
                     std::span<const VideoCommon::BufferImageCopy> copies, bool download) {
    auto map = input;
    if (!map.buffer || map.mapped_span.empty())
        throw std::invalid_argument("D3D12 missing mapped texture staging");
    const auto origin = static_cast<size_t>(map.mapped_span.data() - map.buffer->mapped.data());
    if (map.offset < origin || map.offset - origin > map.mapped_span.size())
        throw std::out_of_range("D3D12 texture staging offset");
    map.mapped_span = map.mapped_span.subspan(map.offset - origin);
    auto& context = runtime->context;
    context.Transition(resource, download ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                          : D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& copy : copies) {
        const auto& extent = copy.image_extent;
        if (!extent.width || !extent.height || !extent.depth)
            throw std::invalid_argument("D3D12 empty image copy");
        const auto rows =
            (u64{extent.height} + host_format.block_height - 1) / host_format.block_height;
        const auto row_length = copy.buffer_row_length ? copy.buffer_row_length : extent.width;
        const auto image_height =
            copy.buffer_image_height ? copy.buffer_image_height : extent.height;
        if (row_length < extent.width || image_height < extent.height ||
            copy.image_subresource.num_layers <= 0)
            throw std::invalid_argument("D3D12 invalid guest texture stride");
        const auto row_stride = CheckedMultiply((u64{row_length} + host_format.block_width - 1) /
                                                    host_format.block_width,
                                                host_format.bytes_per_block);
        const auto slice_stride =
            CheckedMultiply(row_stride, (u64{image_height} + host_format.block_height - 1) /
                                            host_format.block_height);
        // Use the SDK's layout for each transfer extent, then repack guest rows.
        auto desc = resource->handle->GetDesc();
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.DepthOrArraySize =
            static_cast<UINT16>(info.type == VideoCommon::ImageType::e3D ? extent.depth : 1);
        desc.MipLevels = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows{};
        UINT64 row_bytes{}, total{};
        context.Device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_bytes,
                                                &total);
        if (total == UINT64_MAX || !row_bytes || num_rows != rows)
            throw std::runtime_error("D3D12 invalid SDK texture footprint");
        const auto padded_slice = CheckedMultiply(footprint.Footprint.RowPitch, num_rows);
        const auto padded_size = CheckedMultiply(padded_slice, extent.depth);
        for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
            const auto subresource = Subresource(info, copy.image_subresource.base_level,
                                                 copy.image_subresource.base_layer + layer);
            const auto base =
                CheckedAdd(copy.buffer_offset,
                           CheckedMultiply(slice_stride, CheckedMultiply(extent.depth, layer)));
            const auto required = CheckedAdd(
                base, CheckedAdd(CheckedMultiply(slice_stride, extent.depth - 1),
                                 CheckedAdd(CheckedMultiply(row_stride, rows - 1), row_bytes)));
            if (required > map.mapped_span.size())
                throw std::out_of_range("D3D12 guest texture staging bounds");
            const auto level = copy.image_subresource.base_level;
            const auto mip_width = std::max(1u, info.size.width >> level);
            const auto mip_height = std::max(1u, info.size.height >> level);
            const auto mip_depth = info.type == VideoCommon::ImageType::e3D
                                       ? std::max(1u, info.size.depth >> level)
                                       : 1u;
            const auto& offset = copy.image_offset;
            if (offset.x < 0 || offset.y < 0 || offset.z < 0 ||
                u64{static_cast<u32>(offset.x)} + extent.width > mip_width ||
                u64{static_cast<u32>(offset.y)} + extent.height > mip_height ||
                u64{static_cast<u32>(offset.z)} + extent.depth > mip_depth)
                throw std::out_of_range("D3D12 texture copy bounds");
            const bool full_subresource = offset.x == 0 && offset.y == 0 && offset.z == 0 &&
                                          extent.width == mip_width &&
                                          extent.height == mip_height && extent.depth == mip_depth;
            if (host_format.block_width > 1 && !full_subresource &&
                ((static_cast<u32>(offset.x) % host_format.block_width) ||
                 (static_cast<u32>(offset.y) % host_format.block_height) ||
                 (extent.width % host_format.block_width) ||
                 (extent.height % host_format.block_height)))
                throw std::invalid_argument("D3D12 partial BC copy must be block aligned");
            auto aligned = download ? runtime->DownloadStagingBuffer(padded_size)
                                    : runtime->UploadStagingBuffer(padded_size);
            footprint.Offset = aligned.offset;
            D3D12_TEXTURE_COPY_LOCATION linear{};
            linear.pResource = aligned.buffer->handle.Get();
            linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            linear.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION texture{};
            texture.pResource = resource->handle.Get();
            texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            texture.SubresourceIndex = subresource;
            context.Retain(aligned.buffer);
            if (!download) {
                RepackRows(aligned.mapped_span, map.mapped_span.subspan(base), row_bytes, num_rows,
                           extent.depth, footprint.Footprint.RowPitch, padded_slice, row_stride,
                           slice_stride);
                context.MarkWork("upload/copy");
                context.Commands()->CopyTextureRegion(&texture, offset.x, offset.y, offset.z,
                                                      &linear, nullptr);
            } else {
                D3D12_BOX box{static_cast<UINT>(offset.x),
                              static_cast<UINT>(offset.y),
                              static_cast<UINT>(offset.z),
                              static_cast<UINT>(offset.x) + extent.width,
                              static_cast<UINT>(offset.y) + extent.height,
                              static_cast<UINT>(offset.z) + extent.depth};
                context.MarkWork("upload/copy");
                context.Commands()->CopyTextureRegion(&linear, 0, 0, 0, &texture,
                                                      full_subresource ? nullptr : &box);
                // The caller's tight staging stays leased through CPU unpack after the fence.
                context.AfterCompletion([map, aligned, base, slice_stride, row_stride, padded_slice,
                                         row_bytes, num_rows, depth = extent.depth,
                                         pitch = footprint.Footprint.RowPitch] {
                    RepackRows(map.mapped_span.subspan(base), aligned.mapped_span, row_bytes,
                               num_rows, depth, row_stride, slice_stride, pitch, padded_slice);
                });
            }
        }
    }
}
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo& info,
                     VideoCommon::ImageId id, Image& image)
    : ImageViewBase{info, image.info, id, image.gpu_addr}, resource{image.Handle()} {
    const auto view_info = MaxwellFormat(info.format);
    if (!view_info.Supported() || (!VideoCore::Surface::IsPixelFormatASTC(info.format) &&
                                view_info.resource_format != image.HostFormat().resource_format))
        throw std::runtime_error("D3D12 incompatible texture view cast");
    description.Format = static_cast<DXGI_FORMAT>(VideoCore::Surface::IsPixelFormatASTC(info.format)
                                                      ? image.HostFormat().view_format
                                                      : view_info.view_format);
    const auto sources = info.Swizzle();
    UINT mapping[4]{};
    for (size_t i = 0; i < 4; ++i) {
        const auto source = sources[i];
        mapping[i] = source == Tegra::Texture::SwizzleSource::Zero ? 4
                     : source == Tegra::Texture::SwizzleSource::OneFloat ||
                             source == Tegra::Texture::SwizzleSource::OneInt
                         ? 5
                         : static_cast<UINT>(source) - 2;
    }
    description.Shader4ComponentMapping =
        D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2], mapping[3]);
    const auto desc = resource->handle->GetDesc();
    if (range.base.level < 0 || range.extent.levels <= 0 ||
        range.base.level + range.extent.levels > desc.MipLevels || range.base.layer < 0 ||
        range.extent.layers <= 0 ||
        (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
         range.base.layer + range.extent.layers > desc.DepthOrArraySize))
        throw std::out_of_range("D3D12 image view subresources");
    const UINT level = range.base.level;
    const UINT levels = range.extent.levels;
    const UINT first_layer = range.base.layer;
    const UINT layers = range.extent.layers;
    switch (type) {
    case VideoCommon::ImageViewType::e1D:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        description.Texture1D = {level, levels, 0};
        break;
    case VideoCommon::ImageViewType::e1DArray:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        description.Texture1DArray = {level, levels, first_layer, layers, 0};
        break;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::Rect:
        if (resource->handle->GetDesc().DepthOrArraySize == 1) {
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            description.Texture2D = {level, levels, 0, 0};
            break;
        }
        [[fallthrough]];
    case VideoCommon::ImageViewType::e2DArray:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        description.Texture2DArray = {level, levels, first_layer, layers, 0, 0};
        break;
    case VideoCommon::ImageViewType::Cube:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        description.TextureCube = {level, levels, 0};
        break;
    case VideoCommon::ImageViewType::CubeArray:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        description.TextureCubeArray = {level, levels, first_layer, layers / 6, 0};
        break;
    case VideoCommon::ImageViewType::e3D:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        description.Texture3D = {level, levels, 0};
        break;
    default:
        throw std::runtime_error("D3D12 buffer texture views require P3 descriptors");
    }
}
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams& params)
    : ImageViewBase{params} {
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MipLevels = 1;
}
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo& info,
                     const VideoCommon::ImageViewInfo& view, GPUVAddr addr)
    : ImageViewBase{info, view, addr}, buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)} {}
void TextureCacheRuntime::TransitionImageLayout(Image& image) {
    context.Transition(image.Handle(), D3D12_RESOURCE_STATE_COMMON);
}
ComputePasses& TextureCacheRuntime::ComputeHelpers() {
    if (!compute_passes)
        compute_passes = std::make_unique<ComputePasses>(context);
    return *compute_passes;
}
void TextureCacheRuntime::AccelerateImageUpload(
    Image& image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles, u32, u32) {
    if (!VideoCore::Surface::IsPixelFormatASTC(image.info.format))
        throw std::runtime_error("D3D12 accelerated upload currently requires ASTC");
    auto& helpers = ComputeHelpers();
    for (const auto& swizzle : swizzles) {
        const auto params =
            VideoCommon::Accelerated::MakeBlockLinearSwizzle2DParams(swizzle, image.info);
        const u64 size = image.guest_size_bytes - swizzle.buffer_offset;
        auto input = helpers.UploadInput(map.buffer, map.offset + swizzle.buffer_offset, size);
        const std::array buffers{ComputeBufferView{input, 0, size}};
        ComputeTextureView output;
        output.resource = image.Handle();
        output.descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        output.descriptor.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        output.descriptor.Texture2DArray = {static_cast<UINT>(swizzle.level), 0,
                                            static_cast<UINT>(image.info.resources.layers), 0};
        const AstcDecodeParameters constants{
            VideoCore::Surface::DefaultBlockWidth(image.info.format),
            VideoCore::Surface::DefaultBlockHeight(image.info.format),
            params.layer_stride,
            params.block_size,
            params.x_shift,
            params.block_height,
            params.block_height_mask};
        helpers.Run(HelperComputePass::DecodeAstc, constants.Constants(),
                    {swizzle.num_tiles.width, swizzle.num_tiles.height,
                     static_cast<u32>(image.info.resources.layers)},
                    buffers, output);
    }
}
void TextureCacheRuntime::BlitImage(Framebuffer*, ImageView&, ImageView&,
                                    const VideoCommon::Region2D&, const VideoCommon::Region2D&,
                                    Tegra::Engines::Fermi2D::Filter,
                                    Tegra::Engines::Fermi2D::Operation) {
    throw std::runtime_error("D3D12 guest blits are stubbed until P3");
}
void TextureCacheRuntime::CopyImageMSAA(Image&, Image&, std::span<const VideoCommon::ImageCopy>) {
    throw std::runtime_error("D3D12 MSAA copies are unavailable");
}
void TextureCacheRuntime::ConvertImage(Framebuffer*, ImageView&, ImageView&) {
    throw std::runtime_error("D3D12 shader format conversion is unavailable");
}
bool TextureCacheRuntime::ShouldReinterpret(Image& dst, Image& src) {
    return dst.HostFormat().resource_format != src.HostFormat().resource_format;
}
void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src,
                                           std::span<const VideoCommon::ImageCopy> copies) {
    if (ShouldReinterpret(dst, src))
        throw std::runtime_error("D3D12 incompatible image reinterpretation");
    CopyImage(dst, src, copies);
}
void TextureCacheRuntime::CopyImage(Image& dst, Image& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    if (dst.Handle() == src.Handle() || ShouldReinterpret(dst, src))
        throw std::runtime_error("D3D12 image copy needs conversion/scratch");
    context.Transition(src.Handle(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.Transition(dst.Handle(), D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& copy : copies) {
        if (copy.src_subresource.num_layers <= 0 ||
            copy.src_subresource.num_layers != copy.dst_subresource.num_layers ||
            !copy.extent.width || !copy.extent.height || !copy.extent.depth)
            throw std::invalid_argument("D3D12 image copy layer count/extent");
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            D3D12_TEXTURE_COPY_LOCATION source{}, target{};
            source.Type = target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.pResource = src.Handle()->handle.Get();
            target.pResource = dst.Handle()->handle.Get();
            source.SubresourceIndex = Subresource(src.info, copy.src_subresource.base_level,
                                                  copy.src_subresource.base_layer + layer);
            target.SubresourceIndex = Subresource(dst.info, copy.dst_subresource.base_level,
                                                  copy.dst_subresource.base_layer + layer);
            const auto validate_region = [&](const Image& image,
                                             const VideoCommon::Offset3D& offset, s32 level) {
                const u32 width = std::max(1u, image.info.size.width >> level);
                const u32 height = std::max(1u, image.info.size.height >> level);
                const u32 depth = image.info.type == VideoCommon::ImageType::e3D
                                      ? std::max(1u, image.info.size.depth >> level)
                                      : 1u;
                if (offset.x < 0 || offset.y < 0 || offset.z < 0 ||
                    u64{static_cast<u32>(offset.x)} + copy.extent.width > width ||
                    u64{static_cast<u32>(offset.y)} + copy.extent.height > height ||
                    u64{static_cast<u32>(offset.z)} + copy.extent.depth > depth)
                    throw std::out_of_range("D3D12 image copy region");
                return offset.x == 0 && offset.y == 0 && offset.z == 0 &&
                       copy.extent.width == width && copy.extent.height == height &&
                       copy.extent.depth == depth;
            };
            const bool full_source =
                validate_region(src, copy.src_offset, copy.src_subresource.base_level);
            const bool full_target =
                validate_region(dst, copy.dst_offset, copy.dst_subresource.base_level);
            const bool full_copy = full_source && full_target;
            const auto format = src.HostFormat();
            if (format.block_width > 1 && !full_copy &&
                ((static_cast<u32>(copy.src_offset.x) % format.block_width) ||
                 (static_cast<u32>(copy.src_offset.y) % format.block_height) ||
                 (static_cast<u32>(copy.dst_offset.x) % format.block_width) ||
                 (static_cast<u32>(copy.dst_offset.y) % format.block_height) ||
                 (copy.extent.width % format.block_width) ||
                 (copy.extent.height % format.block_height)))
                throw std::invalid_argument("D3D12 partial BC image copy must be block aligned");
            D3D12_BOX box{static_cast<UINT>(copy.src_offset.x),
                          static_cast<UINT>(copy.src_offset.y),
                          static_cast<UINT>(copy.src_offset.z),
                          static_cast<UINT>(copy.src_offset.x) + copy.extent.width,
                          static_cast<UINT>(copy.src_offset.y) + copy.extent.height,
                          static_cast<UINT>(copy.src_offset.z) + copy.extent.depth};
            context.MarkWork("upload/copy");
            context.Commands()->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y,
                                                  copy.dst_offset.z, &source,
                                                  full_copy ? nullptr : &box);
        }
    }
}
void TextureCacheRuntime::TickFrame() {
    upload.Collect();
    readback.Collect();
    ++frame_tick;
    std::erase_if(framebuffers,
                  [this](const auto& entry) { return frame_tick - entry.last_frame > 2; });
}
ResourcePtr TextureCacheRuntime::UploadFramebuffer(Tegra::MaxwellDeviceMemoryManager& memory,
                                                   const Tegra::FramebufferConfig& config) {
    if (!config.width || !config.height || config.stride < config.width || config.stride > 16384 ||
        config.height > 16384)
        throw std::invalid_argument("D3D12 invalid framebuffer dimensions");
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.size = {config.width, config.height, 1};
    info.format = VideoCore::Surface::PixelFormatFromGPUPixelFormat(config.pixel_format);
    const auto format = MaxwellFormat(info.format);
    if (!format.Supported() || format.block_width != 1 || format.depth)
        throw std::runtime_error("D3D12 unsupported framebuffer format");
    const auto address = CheckedAdd(config.address, config.offset);
    auto entry = std::ranges::find(framebuffers, address, &FramebufferEntry::address);
    if (entry == framebuffers.end()) {
        if (framebuffers.size() == 64)
            framebuffers.erase(framebuffers.begin());
        framebuffers.push_back({address, {}, frame_tick});
        entry = std::prev(framebuffers.end());
    }
    entry->last_frame = frame_tick;
    auto& framebuffer = entry->resource;
    if (!framebuffer || framebuffer->handle->GetDesc().Width != config.width ||
        framebuffer->handle->GetDesc().Height != config.height ||
        framebuffer->handle->GetDesc().Format != static_cast<DXGI_FORMAT>(format.resource_format))
        framebuffer = MakeTexture(context, info, format);
    // NVFlinger framebuffer block height matches the existing GL/Vulkan fallback.
    constexpr u32 block_height = 4;
    const auto tiled_size = Tegra::Texture::CalculateSize(
        true, format.bytes_per_block, config.stride, config.height, 1, block_height, 0);
    if (tiled_size > (64ULL << 20))
        throw std::runtime_error("D3D12 framebuffer exceeds staging limit");
    framebuffer_tiled.resize(tiled_size);
    framebuffer_linear.resize(
        CheckedMultiply(CheckedMultiply(config.stride, config.height), format.bytes_per_block));
    memory.ReadBlockUnsafe(address, framebuffer_tiled.data(), tiled_size);
    Tegra::Texture::UnswizzleTexture(framebuffer_linear, framebuffer_tiled, format.bytes_per_block,
                                     config.stride, config.height, 1, block_height, 0);
    const auto footprint = MakeFootprint(format, config.width, config.height);
    auto staging = upload.Request(footprint.size);
    for (u32 y = 0; y < config.height; ++y)
        std::memcpy(staging.mapped_span.data() + y * footprint.row_pitch,
                    framebuffer_linear.data() + u64{y} * config.stride * format.bytes_per_block,
                    footprint.row_bytes);
    context.Transition(framebuffer, D3D12_RESOURCE_STATE_COPY_DEST);
    context.Retain(staging.buffer);
    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
    source.pResource = staging.buffer->handle.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = {staging.offset,
                              {static_cast<DXGI_FORMAT>(format.view_format), config.width,
                               config.height, 1, footprint.row_pitch}};
    target.pResource = framebuffer->handle.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    context.MarkWork("upload/copy");
    context.Commands()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    return framebuffer;
}
} // namespace D3D12
