// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/textures/texture.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
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
    : context{context}, device_memory{memory}, descriptors{context},
      buffer_runtime{context, bindings}, texture_runtime{context},
      buffer_cache{memory, buffer_runtime}, texture_cache{texture_runtime, memory}, m_gpu{gpu} {
    default_pipeline_cache = std::make_unique<PipelineCache>(context, maxwell3d);
    pipeline_cache = default_pipeline_cache.get();
}
RasterizerD3D12::~RasterizerD3D12() = default;

namespace {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

DrawParams MakeDrawParams(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state,
                          u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // Quads are drawn as 6 indexed triangle vertices per quad; the generated indices already
    // include the first vertex (see BufferCacheRuntime::BindQuadIndexBuffer).
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = params.num_vertices < 4 ? 0 : (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}

// Maxwell maps ndc to window space as y_win = translate_y + ndc_y * scale_y. D3D12 viewports
// cannot have a negative height, so a positive Maxwell height (the Vulkan convention) needs the
// vertex shader to negate Y (y_flip), while a negative one maps directly.
struct ViewportResult {
    D3D12_VIEWPORT viewport;
    bool y_flip;
};
ViewportResult MakeViewport(const Maxwell& regs, size_t index) {
    const auto& src = regs.viewport_transform[index];
    float x = src.translate_x - src.scale_x;
    float width = src.scale_x * 2.0f;
    if (width < 0.0f) {
        x += width;
        width = -width;
    }
    float y = src.translate_y - src.scale_y;
    float height = src.scale_y * 2.0f;
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        y += static_cast<f32>(regs.surface_clip.height);
        height = -height;
    }
    if (src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY) {
        y += height;
        height = -height;
    }
    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    if (width == 0.0f)
        width = 1.0f;
    if (height == 0.0f)
        height = 1.0f;
    const bool y_flip = height > 0.0f;
    if (!y_flip) {
        y += height;
        height = -height;
    }
    return {{x, y, std::abs(width), height,
             std::clamp(src.translate_z - src.scale_z * reduce_z, 0.0f, 1.0f),
             std::clamp(src.translate_z + src.scale_z, 0.0f, 1.0f)},
            y_flip};
}

ViewportResult MakeFullViewport(const Maxwell& regs) {
    float x = static_cast<float>(regs.surface_clip.x);
    float y = static_cast<float>(regs.surface_clip.y);
    float width = (std::max)(1.0f, static_cast<float>(regs.surface_clip.width));
    float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
    // Mirrors the Vulkan backend: lower-left origin means a negative height (no shader flip).
    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    return {{x, y, width, height, 0.0f, 1.0f}, !lower_left};
}

D3D12_RECT MakeScissor(const Maxwell& regs, size_t index) {
    if (!regs.viewport_scale_offset_enabled) {
        const LONG x = regs.surface_clip.x;
        const LONG y = regs.surface_clip.y;
        const LONG w = (std::max)(1u, static_cast<u32>(regs.surface_clip.width));
        const LONG h = (std::max)(1u, static_cast<u32>(regs.surface_clip.height));
        const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
        const LONG top = lower_left ? static_cast<LONG>(regs.surface_clip.height) - (y + h) : y;
        return {x, top, x + w, top + h};
    }
    const auto& src = regs.scissor_test[index];
    if (!src.enable)
        return {0, 0, 16384, 16384};
    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = regs.surface_clip.height;
    s32 min_y = lower_left ? (clip_height - src.max_y) : src.min_y.Value();
    s32 max_y = lower_left ? (clip_height - src.min_y) : src.max_y.Value();
    min_y = (std::max)(min_y, 0);
    max_y = (std::max)(max_y, min_y);
    const s32 min_x = src.min_x;
    return {min_x, min_y, (std::max)(min_x, static_cast<s32>(src.max_x)), max_y};
}

std::optional<VideoCore::Surface::PixelFormat> PixelFormatFromImageFormat(
    Shader::ImageFormat format) {
    using VideoCore::Surface::PixelFormat;
    switch (format) {
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    default:
        return std::nullopt;
    }
}

D3D12_SRV_DIMENSION SrvDimension(Shader::TextureType type) {
    switch (type) {
    case Shader::TextureType::Color1D:
        return D3D12_SRV_DIMENSION_TEXTURE1D;
    case Shader::TextureType::ColorArray1D:
        return D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
    case Shader::TextureType::ColorArray2D:
        return D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    case Shader::TextureType::Color3D:
        return D3D12_SRV_DIMENSION_TEXTURE3D;
    case Shader::TextureType::ColorCube:
        return D3D12_SRV_DIMENSION_TEXTURECUBE;
    case Shader::TextureType::ColorArrayCube:
        return D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    case Shader::TextureType::Buffer:
        return D3D12_SRV_DIMENSION_BUFFER;
    default:
        return D3D12_SRV_DIMENSION_TEXTURE2D;
    }
}

constexpr D3D12_RESOURCE_STATES SrvState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES BufferReadState =
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
} // namespace

// Binds everything a draw needs and invokes draw(commands, params-independent). Order matters:
// every descriptor allocation (which may submit the open command list when a ring is full)
// happens BEFORE any per-draw state is recorded.
template <typename Func>
void RasterizerD3D12::PrepareDraw(bool is_indexed, Func&& draw) {
    gpu_memory->FlushCaching();
    if (!pipeline_cache) {
        if (!draw_logged) {
            LOG_WARNING(Render, "D3D12 P3: no pipeline cache registered, draws are skipped");
            draw_logged = true;
        }
        return;
    }
    IGraphicsPipeline* const pipeline = pipeline_cache->CurrentGraphicsPipeline();
    if (!pipeline)
        return;
    ID3D12PipelineState* const pso = pipeline->PipelineState();
    ID3D12RootSignature* const root_signature = pipeline->RootSignature();
    if (!pso || !root_signature)
        return;

    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto& regs = maxwell3d->regs;

    // ---- 1. Gather views/samplers and tell the buffer cache what the shaders use ----
    texture_cache.SynchronizeDescriptors(false);
    StageTableCounts totals{};
    bool has_images = false;
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        uniform_masks[stage] = 0;
        uniform_sizes[stage].fill(0);
        if (const Shader::Info* info = pipeline->StageInfo(stage)) {
            uniform_masks[stage] = info->constant_buffer_mask;
            std::ranges::copy(info->constant_buffer_used_sizes, uniform_sizes[stage].begin());
            const auto counts = CountStageTables(*info);
            totals.cbv += counts.cbv;
            totals.srv += counts.srv;
            totals.uav += counts.uav;
            totals.sampler += counts.sampler;
            has_images |= !info->image_descriptors.empty();
        }
    }
    buffer_cache.SetUniformBuffersState(uniform_masks, &uniform_sizes);

    view_scratch.clear();
    sampler_scratch.clear();
    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        const Shader::Info* info = pipeline->StageInfo(stage);
        if (!info)
            continue;
        buffer_cache.UnbindGraphicsStorageBuffers(stage);
        size_t ssbo_index = 0;
        for (const auto& desc : info->storage_buffers_descriptors) {
            buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index++, desc.cbuf_index,
                                                   desc.cbuf_offset, desc.is_written);
        }
        const auto& cbufs = maxwell3d->state.shader_stages[stage].const_buffers;
        const auto read_handle = [&](const auto& desc, u32 index) {
            const u32 index_offset = index << desc.size_shift;
            const GPUVAddr addr = cbufs[desc.cbuf_index].address + desc.cbuf_offset + index_offset;
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    const GPUVAddr separate = cbufs[desc.secondary_cbuf_index].address +
                                              desc.secondary_cbuf_offset + index_offset;
                    const u32 lhs = gpu_memory->Read<u32>(addr) << desc.shift_left;
                    const u32 rhs = gpu_memory->Read<u32>(separate) << desc.secondary_shift_left;
                    return Tegra::Texture::TexturePair(lhs | rhs, via_header_index);
                }
            }
            return Tegra::Texture::TexturePair(gpu_memory->Read<u32>(addr), via_header_index);
        };
        const auto add_images = [&](const auto& descs, bool blacklist) {
            for (const auto& desc : descs) {
                for (u32 i = 0; i < desc.count; ++i) {
                    view_scratch.push_back({.index = read_handle(desc, i).first,
                                            .blacklist = blacklist,
                                            .id = {}});
                }
            }
        };
        // Same order as the Vulkan backend: texel buffers, image buffers, textures, images.
        add_images(info->texture_buffer_descriptors, false);
        add_images(info->image_buffer_descriptors, false);
        for (const auto& desc : info->texture_descriptors) {
            for (u32 i = 0; i < desc.count; ++i) {
                const auto handle = read_handle(desc, i);
                view_scratch.push_back({.index = handle.first, .blacklist = false, .id = {}});
                sampler_scratch.push_back(texture_cache.GetSamplerId(handle.second, false));
            }
        }
        for (const auto& desc : info->image_descriptors) {
            for (u32 i = 0; i < desc.count; ++i)
                view_scratch.push_back(
                    {.index = read_handle(desc, i).first, .blacklist = desc.is_written, .id = {}});
        }
    }
    texture_cache.FillImageViews(std::span(view_scratch.data(), view_scratch.size()), false,
                                 has_images);

    // Texel/image buffers are bound through the buffer cache from the resolved views.
    {
        const VideoCommon::ImageViewInOut* it = view_scratch.data();
        for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
            const Shader::Info* info = pipeline->StageInfo(stage);
            if (!info)
                continue;
            buffer_cache.UnbindGraphicsTextureBuffers(stage);
            size_t index = 0;
            const auto add_buffers = [&](const auto& descs, bool is_image) {
                for (const auto& desc : descs) {
                    for (u32 i = 0; i < desc.count; ++i, ++it, ++index) {
                        ImageView& view = texture_cache.GetImageView(it->id);
                        auto format = view.format;
                        bool is_written = false;
                        if constexpr (requires { desc.is_written; }) {
                            is_written = desc.is_written;
                            if (const auto explicit_format =
                                    PixelFormatFromImageFormat(desc.format))
                                format = *explicit_format;
                        }
                        buffer_cache.BindGraphicsTextureBuffer(stage, index, view.GpuAddr(),
                                                               view.BufferSize(), format,
                                                               is_written, is_image);
                    }
                }
            };
            add_buffers(info->texture_buffer_descriptors, false);
            add_buffers(info->image_buffer_descriptors, true);
            it += Shader::NumDescriptors(info->texture_descriptors) +
                  Shader::NumDescriptors(info->image_descriptors);
        }
    }

    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    buffer_cache.BindHostGeometryBuffers(is_indexed);
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        bindings.stages[stage].Clear();
        if (!pipeline->StageInfo(stage))
            continue;
        bindings.current_stage = stage;
        buffer_cache.BindHostStageBuffers(stage);
    }
    if (buffer_cache.any_buffer_uploaded) {
        buffer_runtime.PostCopyBarrier();
        buffer_cache.any_buffer_uploaded = false;
    }
    texture_cache.UpdateRenderTargets(false);
    Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    if (!framebuffer)
        return;
    const u32 num_rts = std::min<u32>(regs.rt_control.count, VideoCommon::NUM_RT);

    // ---- 2. Allocate every descriptor this draw needs (may submit the command list) ----
    const u32 cbv_size = TableSize(totals.cbv);
    const u32 srv_size = TableSize(totals.srv);
    const u32 uav_size = TableSize(totals.uav);
    const auto view_block = descriptors.AllocateViews(cbv_size + srv_size + uav_size);
    const auto cbv_table = view_block.Slice(0, cbv_size);
    const auto srv_table = view_block.Slice(cbv_size, srv_size);
    const auto uav_table = view_block.Slice(cbv_size + srv_size, uav_size);
    const auto sampler_table = descriptors.AllocateSamplers(TableSize(totals.sampler));
    const auto rtv_base = descriptors.AllocateRtv(std::max(num_rts, 1u));
    ImageView* const depth_view = framebuffer->Depth();
    const bool has_depth = depth_view && depth_view->ImageHandle();
    const auto dsv_handle = has_depth ? descriptors.AllocateDsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};

    // ---- 3. Fill the tables and collect resource states ----
    state_scratch.clear();
    const auto need = [this](const ResourcePtr& resource, D3D12_RESOURCE_STATES state) {
        if (!resource)
            return;
        for (auto& [known, known_state] : state_scratch) {
            if (known != resource)
                continue;
            known_state = (known_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
                           state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                              ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                              : (known_state | state);
            return;
        }
        state_scratch.emplace_back(resource, state);
    };
    u32 cbv_i = 0, srv_i = 0, uav_i = 0, smp_i = 0;
    const VideoCommon::ImageViewInOut* view_it = view_scratch.data();
    const VideoCommon::SamplerId* sampler_it = sampler_scratch.data();
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        const Shader::Info* info = pipeline->StageInfo(stage);
        if (!info)
            continue;
        const auto& bound = bindings.stages[stage];
        size_t uniform_it = 0, storage_it = 0, texel_it = 0;
        // Constant buffers, in the order of constant_buffer_descriptors.
        for (const auto& desc : info->constant_buffer_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++cbv_i) {
                if (uniform_it < bound.uniform.size()) {
                    const auto& buffer = bound.uniform[uniform_it++];
                    descriptors.WriteCbv(cbv_table, cbv_i, buffer);
                    need(buffer.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                } else {
                    descriptors.WriteNullCbv(cbv_table, cbv_i);
                }
            }
        }
        // Textures (+ samplers), then texel buffers.
        const u32 texel_count = Shader::NumDescriptors(info->texture_buffer_descriptors);
        const u32 image_buffer_count = Shader::NumDescriptors(info->image_buffer_descriptors);
        const VideoCommon::ImageViewInOut* texture_views = view_it + texel_count + image_buffer_count;
        for (const auto& desc : info->texture_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++srv_i, ++smp_i) {
                ImageView& view = texture_cache.GetImageView((texture_views++)->id);
                if (descriptors.WriteImageSrv(srv_table, srv_i, view, SrvDimension(desc.type)))
                    need(view.ImageHandle(), SrvState);
                descriptors.WriteSampler(sampler_table, smp_i,
                                         texture_cache.GetSampler(*sampler_it++));
            }
        }
        for (u32 i = 0; i < texel_count; ++i, ++srv_i) {
            if (texel_it < bound.texel.size()) {
                const auto& buffer = bound.texel[texel_it++];
                descriptors.WriteTexelSrv(srv_table, srv_i, buffer);
                need(buffer.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            } else {
                descriptors.WriteNullSrv(srv_table, srv_i, D3D12_SRV_DIMENSION_BUFFER);
            }
        }
        // Storage buffers, image buffers, images.
        for (const auto& desc : info->storage_buffers_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++uav_i) {
                if (storage_it < bound.storage.size()) {
                    const auto& buffer = bound.storage[storage_it++];
                    descriptors.WriteStorageUav(uav_table, uav_i, buffer);
                    need(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                } else {
                    descriptors.WriteNullUav(uav_table, uav_i);
                }
            }
        }
        for (u32 i = 0; i < image_buffer_count; ++i, ++uav_i) {
            if (texel_it < bound.texel.size()) {
                const auto& buffer = bound.texel[texel_it++];
                descriptors.WriteTexelUav(uav_table, uav_i, buffer);
                need(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                descriptors.WriteNullUav(uav_table, uav_i);
            }
        }
        for (const auto& desc : info->image_descriptors) {
            // TODO(P4): typed image UAVs (needs ALLOW_UNORDERED_ACCESS on cached images).
            if (!image_uav_logged) {
                LOG_WARNING(Render, "D3D12 P3: shader storage images are bound as null UAVs");
                image_uav_logged = true;
            }
            for (u32 i = 0; i < desc.count; ++i, ++uav_i)
                descriptors.WriteNullUav(uav_table, uav_i);
        }
        view_it += texel_count + image_buffer_count +
                   Shader::NumDescriptors(info->texture_descriptors) +
                   Shader::NumDescriptors(info->image_descriptors);
    }
    for (u32 i = cbv_i; i < cbv_size; ++i)
        descriptors.WriteNullCbv(cbv_table, i);
    for (u32 i = srv_i; i < srv_size; ++i)
        descriptors.WriteNullSrv(srv_table, i, D3D12_SRV_DIMENSION_TEXTURE2D);
    for (u32 i = uav_i; i < uav_size; ++i)
        descriptors.WriteNullUav(uav_table, i);
    for (u32 i = smp_i; i < sampler_table.count; ++i)
        descriptors.WriteNullSampler(sampler_table, i);

    // Geometry buffers.
    auto& geometry = bindings.geometry;
    std::array<D3D12_VERTEX_BUFFER_VIEW, GeometryBindings::MaxVertexBuffers> vertex_views{};
    u32 num_vertex_views = 0;
    for (u32 i = 0; i < GeometryBindings::MaxVertexBuffers; ++i) {
        const auto& slot = geometry.vertex[i];
        if (!slot.resource || !slot.size)
            continue;
        vertex_views[i] = {slot.resource->handle->GetGPUVirtualAddress() + slot.offset,
                           static_cast<UINT>(slot.size), slot.stride};
        need(slot.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        num_vertex_views = i + 1;
    }
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const bool quads = draw_state.topology == Maxwell::PrimitiveTopology::Quads ||
                       draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip;
    const bool needs_index = is_indexed || quads;
    D3D12_INDEX_BUFFER_VIEW index_view{};
    if (needs_index) {
        if (!geometry.index || !geometry.index_size || geometry.index_unsupported) {
            if (geometry.index_unsupported && !quad_logged) {
                LOG_WARNING(Render, "D3D12 P3: 8-bit/indexed-quad index conversion not "
                                    "implemented, draw skipped");
                quad_logged = true;
            }
            return;
        }
        index_view = {geometry.index->handle->GetGPUVirtualAddress() + geometry.index_offset,
                      static_cast<UINT>(geometry.index_size),
                      geometry.index_is_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
        need(geometry.index, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
    // Render targets.
    std::array<ImageView*, VideoCommon::NUM_RT> color_views{};
    const SIZE_T rtv_stride =
        context.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (u32 slot = 0; slot < num_rts; ++slot) {
        ImageView* view = framebuffer->Color(framebuffer->DrawBuffer(slot));
        color_views[slot] = view && view->ImageHandle() ? view : nullptr;
        descriptors.CreateRtv({rtv_base.ptr + slot * rtv_stride}, color_views[slot]);
    }
    if (has_depth)
        descriptors.CreateDsv(dsv_handle, *depth_view);

    // ---- 4. Record state (no more allocations from here on) ----
    auto* const commands = context.Commands();
    descriptors.SetHeaps(commands);
    for (const auto& [resource, state] : state_scratch)
        context.Transition(resource, state);
    for (u32 slot = 0; slot < num_rts; ++slot) {
        if (color_views[slot])
            context.Transition(color_views[slot]->ImageHandle(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (has_depth)
        context.Transition(depth_view->ImageHandle(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

    commands->SetGraphicsRootSignature(root_signature);
    commands->SetPipelineState(pso);
    commands->IASetPrimitiveTopology(pipeline->Topology());
    commands->SetGraphicsRootDescriptorTable(1, cbv_table.gpu);
    commands->SetGraphicsRootDescriptorTable(2, srv_table.gpu);
    commands->SetGraphicsRootDescriptorTable(3, uav_table.gpu);
    commands->SetGraphicsRootDescriptorTable(4, sampler_table.gpu);

    commands->OMSetRenderTargets(num_rts, num_rts ? &rtv_base : nullptr, TRUE,
                                 has_depth ? &dsv_handle : nullptr);
    std::array<D3D12_VIEWPORT, Maxwell::NumViewports> viewports{};
    std::array<D3D12_RECT, Maxwell::NumViewports> scissors{};
    bool y_flip = false;
    for (size_t i = 0; i < Maxwell::NumViewports; ++i) {
        const auto result = regs.viewport_scale_offset_enabled ? MakeViewport(regs, i)
                                                                : MakeFullViewport(regs);
        viewports[i] = result.viewport;
        scissors[i] = MakeScissor(regs, i);
        if (i == 0)
            y_flip = result.y_flip;
    }
    commands->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    commands->RSSetScissorRects(static_cast<UINT>(scissors.size()), scissors.data());
    commands->IASetVertexBuffers(0, num_vertex_views, vertex_views.data());
    if (needs_index)
        commands->IASetIndexBuffer(&index_view);

    const auto size = framebuffer->Size();
    const auto words = pipeline->RootConstants({size.width, size.height, y_flip});
    commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(words.size()), words.data(), 0);

    draw(commands);
    for (auto& stage : bindings.stages)
        stage.Clear(); // drop upload-page references so the staging ring can recycle them
}

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) {
    PrepareDraw(is_indexed, [&](ID3D12GraphicsCommandList* commands) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        const DrawParams params = MakeDrawParams(draw_state, instance_count, is_indexed);
        if (!params.num_vertices || !params.num_instances)
            return;
        if (params.is_indexed) {
            commands->DrawIndexedInstanced(params.num_vertices, params.num_instances,
                                           params.first_index, static_cast<INT>(params.base_vertex),
                                           params.base_instance);
        } else {
            commands->DrawInstanced(params.num_vertices, params.num_instances, params.base_vertex,
                                    params.base_instance);
        }
    });
    m_gpu.TickWork();
}

ID3D12CommandSignature* RasterizerD3D12::IndirectSignature(bool indexed) {
    auto& signature = indexed ? indexed_signature : draw_signature;
    if (!signature) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
                                : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &argument;
        Check(context.Device()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)),
              "CreateCommandSignature");
    }
    return signature.Get();
}

void RasterizerD3D12::DrawIndirect() {
    const auto& params = maxwell3d->draw_manager.indirect_state;
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, [&](ID3D12GraphicsCommandList* commands) {
        if (params.is_byte_count) {
            LOG_WARNING(Render, "D3D12 P3: byte-count indirect draw (transform feedback) skipped");
            return;
        }
        const auto [arguments, argument_offset] = buffer_cache.GetDrawIndirectBuffer();
        if (!arguments->Handle())
            return;
        context.Transition(arguments->Handle(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto* const signature = IndirectSignature(params.is_indexed);
        const size_t native_stride =
            params.is_indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
        if (params.include_count && params.stride == native_stride) {
            const auto [count, count_offset] = buffer_cache.GetDrawIndirectCount();
            if (!count->Handle())
                return;
            context.Transition(count->Handle(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            commands->ExecuteIndirect(signature, static_cast<UINT>(params.max_draw_counts),
                                      arguments->Handle()->handle.Get(), argument_offset,
                                      count->Handle()->handle.Get(), count_offset);
            return;
        }
        // Arbitrary guest stride (or no count buffer): one ExecuteIndirect per command.
        for (size_t i = 0; i < params.max_draw_counts; ++i) {
            commands->ExecuteIndirect(signature, 1, arguments->Handle()->handle.Get(),
                                      argument_offset + i * params.stride, nullptr, 0);
        }
    });
    buffer_cache.SetDrawIndirect(nullptr);
    m_gpu.TickWork();
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
                                                u32 size) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}
void RasterizerD3D12::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}
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
