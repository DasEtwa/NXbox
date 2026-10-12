// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_descriptor.h"

#include <algorithm>
#include <stdexcept>
#include "common/alignment.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/textures/texture.h"

namespace D3D12 {
using Tegra::Texture::DepthCompareFunc;
using Tegra::Texture::TextureFilter;
using Tegra::Texture::TextureMipmapFilter;
using Tegra::Texture::WrapMode;

DXGI_FORMAT DsvFormat(DXGI_FORMAT view_format) {
    switch (view_format) {
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
        return view_format;
    }
}

namespace {
D3D12_TEXTURE_ADDRESS_MODE AddressMode(WrapMode mode) {
    switch (mode) {
    case WrapMode::Wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case WrapMode::Mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case WrapMode::Border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case WrapMode::MirrorOnceClampToEdge:
    case WrapMode::MirrorOnceBorder:
    case WrapMode::MirrorOnceClampOGL:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: // ClampToEdge, Clamp
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}
} // namespace

D3D12_SAMPLER_DESC MakeSamplerDesc(const Tegra::Texture::TSCEntry& entry) {
    D3D12_SAMPLER_DESC desc{};
    const u32 min_linear = entry.min_filter == TextureFilter::Linear ? 1 : 0;
    const u32 mag_linear = entry.mag_filter == TextureFilter::Linear ? 1 : 0;
    const u32 mip_linear = entry.mipmap_filter == TextureMipmapFilter::Linear ? 1 : 0;
    u32 reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (entry.depth_compare_enabled) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    } else if (entry.reduction_filter == Tegra::Texture::SamplerReduction::Min) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    } else if (entry.reduction_filter == Tegra::Texture::SamplerReduction::Max) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    }
    const float anisotropy = std::clamp(entry.MaxAnisotropy(), 1.0f, 16.0f);
    desc.Filter = anisotropy > 1.0f
                      ? static_cast<D3D12_FILTER>(D3D12_ANISOTROPIC_FILTERING_BIT |
                                                  D3D12_ENCODE_BASIC_FILTER(1, 1, 1, reduction))
                      : static_cast<D3D12_FILTER>(
                            D3D12_ENCODE_BASIC_FILTER(min_linear, mag_linear, mip_linear, reduction));
    desc.AddressU = AddressMode(entry.wrap_u);
    desc.AddressV = AddressMode(entry.wrap_v);
    desc.AddressW = AddressMode(entry.wrap_p);
    desc.MipLODBias = entry.LodBias();
    desc.MaxAnisotropy = static_cast<UINT>(anisotropy);
    desc.ComparisonFunc = entry.depth_compare_enabled
                              ? static_cast<D3D12_COMPARISON_FUNC>(
                                    static_cast<u32>(entry.depth_compare_func.Value()) + 1)
                              : D3D12_COMPARISON_FUNC_NEVER;
    const auto border = entry.srgb_conversion ? entry.SrgbBorderColor() : entry.BorderColor();
    std::copy(border.begin(), border.end(), desc.BorderColor);
    desc.MinLOD = entry.MinLod();
    // yuzu convention: no mip filter means "sample level 0 only".
    desc.MaxLOD = entry.mipmap_filter == TextureMipmapFilter::None ? 0.25f : entry.MaxLod();
    return desc;
}

D3D12DescriptorAllocator::D3D12DescriptorAllocator(Context& context) : context{context} {
    Init(views, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ViewHeapSize, true);
    Init(samplers, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SamplerHeapSize, true);
    Init(rtvs, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtvHeapSize, false);
    Init(dsvs, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DsvHeapSize, false);
}

void D3D12DescriptorAllocator::Init(Ring& ring, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity,
                                    bool visible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(context.Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&ring.heap)),
          "CreateDescriptorHeap", context.Device());
    ring.stride = context.Device()->GetDescriptorHandleIncrementSize(type);
    ring.cpu = ring.heap->GetCPUDescriptorHandleForHeapStart();
    ring.shader_visible = visible;
    if (visible)
        ring.gpu = ring.heap->GetGPUDescriptorHandleForHeapStart();
}

void D3D12DescriptorAllocator::Reserve(u32 view_count, u32 sampler_count, u32 rtv_count,
                                       u32 dsv_count) {
    const std::array rings{&views, &samplers, &rtvs, &dsvs};
    const std::array counts{view_count, sampler_count, rtv_count, dsv_count};
    for (size_t i = 0; i < rings.size(); ++i) {
        if (counts[i] > rings[i]->math.Capacity())
            throw std::runtime_error("D3D12 descriptor batch larger than its heap");
    }
    for (;;) {
        // Retire callbacks before the preflight, then keep this recording open.
        // Every subsequent allocation in this batch must use the same fence tick.
        context.Commands();
        bool ready = true;
        for (size_t i = 0; i < rings.size(); ++i) {
            auto& ring = *rings[i];
            while (const auto oldest = ring.math.OldestTick()) {
                if (!context.IsFree(*oldest))
                    break;
                ring.math.Retire(*oldest);
            }
            ready &= counts[i] == 0 || ring.math.CanAllocate(counts[i]);
        }
        if (ready)
            return;
        context.Finish();
    }
}

void D3D12DescriptorAllocator::SetHeaps(ID3D12GraphicsCommandList* commands) {
    ID3D12DescriptorHeap* heaps[] = {views.heap.Get(), samplers.heap.Get()};
    commands->SetDescriptorHeaps(2, heaps);
}

DescriptorTable D3D12DescriptorAllocator::Allocate(Ring& ring, u32 count) {
    for (;;) {
        while (const auto oldest = ring.math.OldestTick()) {
            if (!context.IsFree(*oldest))
                break;
            ring.math.Retire(*oldest);
        }
        if (const auto offset = ring.math.Allocate(count, context.CurrentTick())) {
            DescriptorTable table;
            table.count = count;
            table.stride = ring.stride;
            table.cpu = {ring.cpu.ptr + static_cast<SIZE_T>(*offset) * ring.stride};
            if (ring.shader_visible)
                table.gpu = {ring.gpu.ptr + static_cast<UINT64>(*offset) * ring.stride};
            return table;
        }
        const auto oldest = ring.math.OldestTick();
        if (!oldest)
            throw std::runtime_error("D3D12 descriptor table larger than its heap");
        // Everything live belongs to the open command list: close it so it can retire.
        if (*oldest >= context.CurrentTick())
            context.Submit();
        else
            context.Wait(*oldest);
    }
}

DescriptorTable D3D12DescriptorAllocator::AllocateViews(u32 count) {
    return Allocate(views, count);
}
DescriptorTable D3D12DescriptorAllocator::AllocateSamplers(u32 count) {
    return Allocate(samplers, count);
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::AllocateRtv(u32 count) {
    return Allocate(rtvs, count).cpu;
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::AllocateDsv() {
    return Allocate(dsvs, 1).cpu;
}

void D3D12DescriptorAllocator::WriteNullCbv(const DescriptorTable& table, u32 slot) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    context.Device()->CreateConstantBufferView(&desc, table.Slot(slot));
}
void D3D12DescriptorAllocator::WriteNullSrv(const DescriptorTable& table, u32 slot,
                                            D3D12_SRV_DIMENSION dimension) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = dimension;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (dimension) {
    case D3D12_SRV_DIMENSION_BUFFER:
        desc.Buffer.NumElements = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        desc.Texture1D.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        desc.Texture1DArray.MipLevels = desc.Texture1DArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        desc.Texture2DArray.MipLevels = desc.Texture2DArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        desc.Texture3D.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        desc.TextureCube.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        desc.TextureCubeArray.MipLevels = desc.TextureCubeArray.NumCubes = 1;
        break;
    default:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        break;
    }
    context.Device()->CreateShaderResourceView(nullptr, &desc, table.Slot(slot));
}
void D3D12DescriptorAllocator::WriteNullUav(const DescriptorTable& table, u32 slot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    context.Device()->CreateUnorderedAccessView(nullptr, nullptr, &desc, table.Slot(slot));
}
void D3D12DescriptorAllocator::WriteNullSampler(const DescriptorTable& table, u32 slot) {
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    context.Device()->CreateSampler(&desc, table.Slot(slot));
}

void D3D12DescriptorAllocator::WriteCbv(const DescriptorTable& table, u32 slot,
                                        const BoundBuffer& buffer) {
    if (!buffer.resource || !buffer.size) {
        WriteNullCbv(table, slot);
        return;
    }
    const u64 width = buffer.resource->handle->GetDesc().Width;
    u64 size = Common::AlignUp<u64>(buffer.size, 256);
    if (buffer.offset >= width) {
        size = 0;
    } else if (buffer.offset + size > width) {
        // CBV ranges must be 256-byte multiples: drop the (sub-256) tail past the allocation.
        size = (width - buffer.offset) & ~u64{255};
    }
    if (!size) {
        WriteNullCbv(table, slot);
        return;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    desc.BufferLocation = buffer.resource->handle->GetGPUVirtualAddress() + buffer.offset;
    desc.SizeInBytes = static_cast<UINT>(size);
    context.Device()->CreateConstantBufferView(&desc, table.Slot(slot));
}

void D3D12DescriptorAllocator::WriteStorageUav(const DescriptorTable& table, u32 slot,
                                               const BoundBuffer& buffer) {
    const u32 size = buffer.size & ~3u;
    if (!buffer.resource || !size) {
        WriteNullUav(table, slot);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = buffer.offset / 4;
    desc.Buffer.NumElements = size / 4;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    context.Device()->CreateUnorderedAccessView(buffer.resource->handle.Get(), nullptr, &desc,
                                                table.Slot(slot));
}

void D3D12DescriptorAllocator::WriteTexelSrv(const DescriptorTable& table, u32 slot,
                                             const BoundBuffer& buffer) {
    const auto format = MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
    if (!buffer.resource || !format.Supported() || !format.bytes_per_block ||
        buffer.size < format.bytes_per_block) {
        WriteNullSrv(table, slot, D3D12_SRV_DIMENSION_BUFFER);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(format.view_format);
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = buffer.offset / format.bytes_per_block;
    desc.Buffer.NumElements = buffer.size / format.bytes_per_block;
    context.Device()->CreateShaderResourceView(buffer.resource->handle.Get(), &desc,
                                               table.Slot(slot));
}

void D3D12DescriptorAllocator::WriteTexelUav(const DescriptorTable& table, u32 slot,
                                             const BoundBuffer& buffer) {
    const auto format = MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
    if (!buffer.resource || !format.Supported() || !format.bytes_per_block ||
        buffer.size < format.bytes_per_block) {
        WriteNullUav(table, slot);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(format.view_format);
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = buffer.offset / format.bytes_per_block;
    desc.Buffer.NumElements = buffer.size / format.bytes_per_block;
    context.Device()->CreateUnorderedAccessView(buffer.resource->handle.Get(), nullptr, &desc,
                                                table.Slot(slot));
}

bool D3D12DescriptorAllocator::WriteImageSrv(const DescriptorTable& table, u32 slot,
                                             const ImageView& view,
                                             D3D12_SRV_DIMENSION fallback_dimension) {
    const auto resource = view.ImageHandle();
    if (!resource) {
        WriteNullSrv(table, slot, fallback_dimension);
        return false;
    }
    context.Device()->CreateShaderResourceView(resource->handle.Get(), &view.Description(),
                                               table.Slot(slot));
    return true;
}

void D3D12DescriptorAllocator::WriteSampler(const DescriptorTable& table, u32 slot,
                                            const Sampler& sampler) {
    const auto desc = MakeSamplerDesc(sampler.Entry());
    context.Device()->CreateSampler(&desc, table.Slot(slot));
}

void D3D12DescriptorAllocator::CreateRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                         const ImageView* view_ptr) {
    const auto resource = view_ptr ? view_ptr->ImageHandle() : nullptr;
    if (!resource) {
        // Null render target: a valid RTV that discards writes.
        D3D12_RENDER_TARGET_VIEW_DESC null_desc{};
        null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        null_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        context.Device()->CreateRenderTargetView(nullptr, &null_desc, handle);
        return;
    }
    const ImageView& view = *view_ptr;
    const auto desc = resource->handle->GetDesc();
    const UINT level = static_cast<UINT>(view.range.base.level);
    const UINT layer = static_cast<UINT>(view.range.base.layer);
    const UINT layers = static_cast<UINT>(view.range.extent.layers);
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = view.Description().Format;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rtv.Texture3D = {level, 0, static_cast<UINT>(-1)};
    } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        rtv.Texture1DArray = {level, layer, layers};
    } else {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray = {level, layer, layers, 0};
    }
    context.Device()->CreateRenderTargetView(resource->handle.Get(), &rtv, handle);
}

void D3D12DescriptorAllocator::CreateDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                         const ImageView& view) {
    const auto resource = view.ImageHandle();
    if (!resource)
        throw std::runtime_error("D3D12 depth view without image");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DsvFormat(view.Description().Format);
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    dsv.Texture2DArray = {static_cast<UINT>(view.range.base.level),
                          static_cast<UINT>(view.range.base.layer),
                          static_cast<UINT>(view.range.extent.layers)};
    context.Device()->CreateDepthStencilView(resource->handle.Get(), &dsv, handle);
}
bool D3D12DescriptorAllocator::WriteImageUav(const DescriptorTable& table, u32 slot,
                                             const ImageView& view, DXGI_FORMAT format) {
    if (!view.ImageHandle()) {
        WriteNullUav(table, slot);
        return false;
    }
    if (!(view.ImageHandle()->handle->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        throw std::runtime_error("D3D12 image does not support UAV access");
    const auto& srv = view.Description();
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = format;
    switch (srv.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        desc.Texture1D.MipSlice = srv.Texture1D.MostDetailedMip;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray = {srv.Texture1DArray.MostDetailedMip,
                               srv.Texture1DArray.FirstArraySlice, srv.Texture1DArray.ArraySize};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2D = {srv.Texture2D.MostDetailedMip, 0};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {srv.Texture2DArray.MostDetailedMip,
                               srv.Texture2DArray.FirstArraySlice, srv.Texture2DArray.ArraySize, 0};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {srv.Texture3D.MostDetailedMip, 0, static_cast<UINT>(-1)};
        break;
    default:
        throw std::runtime_error("D3D12 unsupported storage image dimension");
    }
    context.Device()->CreateUnorderedAccessView(view.ImageHandle()->handle.Get(), nullptr, &desc,
                                                table.Slot(slot));
    return true;
}
} // namespace D3D12
