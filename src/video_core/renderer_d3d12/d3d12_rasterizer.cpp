// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_buffer_view.h"
#include "video_core/renderer_d3d12/d3d12_feedback.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/textures/texture.h"

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
      buffer_cache{memory, buffer_runtime}, texture_cache{texture_runtime, memory},
      index_converter{context}, clear_renderer{context}, compute_dispatch{context},
      transform_feedback{context}, query_runtime{context, memory, buffer_cache, transform_feedback},
      query_cache{gpu, *this, memory, query_runtime},
      fence_manager{*this, gpu, texture_cache, buffer_cache, query_cache, context}, m_gpu{gpu} {
    default_pipeline_cache = std::make_unique<PipelineCache>(context, memory);
    pipeline_cache = default_pipeline_cache.get();
}
RasterizerD3D12::~RasterizerD3D12() {
    // Query callbacks capture the cache. Drain while every cache/runtime is alive.
    try {
        fence_manager.SignalReference();
        fence_manager.WaitPendingFences(true);
        context.Finish();
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 rasterizer teardown: {}", error.what());
    }
}

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

void RasterizerD3D12::SkipDraw(const char* reason) {
    ++skipped_draws;
    if (skipped_draws <= 4 && (feedback_frame < 8 || feedback_frame % 120 == 0))
        Diagnostic(std::string{"D3D12_DRAW_SKIP reason="} + reason);
    for (auto& stage : bindings.stages)
        stage.Clear();
}

ResourcePtr RasterizerD3D12::CopyFeedback(const ResourcePtr& source) {
    auto desc = source->handle->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    FeedbackTexture* selected{};
    for (auto& entry : feedback_pool) {
        if (entry.draw == feedback_draw && entry.source == source.get())
            return entry.resource;
        const auto other = entry.resource->handle->GetDesc();
        if (entry.draw != feedback_draw && desc.Dimension == other.Dimension &&
            desc.Alignment == other.Alignment && desc.Width == other.Width &&
            desc.Height == other.Height && desc.DepthOrArraySize == other.DepthOrArraySize &&
            desc.MipLevels == other.MipLevels && desc.Format == other.Format &&
            desc.SampleDesc.Count == other.SampleDesc.Count &&
            desc.SampleDesc.Quality == other.SampleDesc.Quality && desc.Layout == other.Layout &&
            desc.Flags == other.Flags) {
            selected = &entry;
        }
    }
    if (!selected) {
        // Evict inactive shapes before allocating. Pending command lists still retain
        // them until their fence completes; never bypass the global memory budget.
        std::erase_if(feedback_pool, [this](const auto& entry) {
            return entry.frame != feedback_frame && entry.draw != feedback_draw;
        });
        const auto allocation = context.Device()->GetResourceAllocationInfo(0, 1, &desc);
        u64 pool_bytes{};
        for (const auto& entry : feedback_pool)
            pool_bytes += entry.resource->allocation_size;
        constexpr u64 PoolLimit = 64ULL << 20;
        if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > PoolLimit ||
            pool_bytes > PoolLimit - allocation.SizeInBytes ||
            allocation.SizeInBytes > MemoryBudget::Limit - context.MemoryUsage())
            return {};
        feedback_pool.push_back({context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT,
                                                        D3D12_RESOURCE_STATE_COPY_DEST)});
        selected = &feedback_pool.back();
    }
    selected->source = source.get();
    selected->draw = feedback_draw;
    selected->frame = feedback_frame;
    context.Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.Transition(selected->resource, D3D12_RESOURCE_STATE_COPY_DEST);
    // Preserve every mip, layer and depth/stencil plane: SRVs can span more
    // subresources than the attachment view (including mip-generation feedback).
    context.Commands()->CopyResource(selected->resource->handle.Get(), source->handle.Get());
    context.MarkWork("attachment feedback copies");
    ++feedback_copies;
    return selected->resource;
}

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
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto& regs = maxwell3d->regs;
    texture_cache.SynchronizeDescriptors(false);
    texture_cache.UpdateRenderTargets(false);
    auto* const cached_framebuffer = texture_cache.GetFramebuffer();
    if (!cached_framebuffer)
        return;
    Framebuffer framebuffer_snapshot = *cached_framebuffer;
    Framebuffer* const framebuffer = &framebuffer_snapshot;
    Pso::FramebufferFormats formats{};
    std::array<ImageView*, VideoCommon::NUM_RT> color_views{};
    const u32 guest_rts = std::min<u32>(regs.rt_control.count, VideoCommon::NUM_RT);
    for (u32 slot = 0; slot < guest_rts; ++slot) {
        auto* view = framebuffer->Color(framebuffer->DrawBuffer(slot));
        if (view && view->ImageHandle()) {
            color_views[slot] = view;
            formats.colors[slot] = static_cast<u32>(view->Description().Format);
        }
    }
    ImageView* const depth_view = framebuffer->Depth();
    const bool has_depth = depth_view && depth_view->ImageHandle();
    if (has_depth)
        formats.depth = static_cast<u32>(DsvFormat(depth_view->Description().Format));
    Pso::GraphicsPsoState attachment_state{};
    Pso::ApplyFramebufferFormats(attachment_state, formats);
    const u32 num_rts = attachment_state.num_render_targets;
    query_runtime.PrepareDraw();
    IGraphicsPipeline* const pipeline = pipeline_cache->CurrentGraphicsPipeline(formats);
    if (!pipeline)
        return;
    ID3D12PipelineState* const pso = pipeline->PipelineState();
    ID3D12RootSignature* const root_signature = pipeline->RootSignature();
    if (!pso || !root_signature)
        return;

    // ---- 1. Gather views/samplers and tell the buffer cache what the shaders use ----
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

    // Guest mappings can shrink between cache bindings. Do not hand invalid
    // buffer ranges to the descriptor API or poison the whole GPU thread.
    for (const auto& stage : bindings.stages) {
        const auto valid = [](const auto& buffer, u64 alignment) {
            return !buffer.resource || !buffer.size ||
                   (buffer.offset % alignment == 0 &&
                    IsValidBufferView(buffer.resource->handle->GetGPUVirtualAddress(),
                                      buffer.resource->handle->GetDesc().Width, buffer.offset,
                                      buffer.size));
        };
        for (const auto& buffer : stage.uniform) {
            if (!valid(buffer, 256)) {
                SkipDraw("invalid constant buffer view");
                return;
            }
        }
        for (const auto& buffer : stage.storage) {
            if (!valid(buffer, 4)) {
                SkipDraw("invalid storage buffer view");
                return;
            }
        }
        for (const auto& buffer : stage.texel) {
            const auto format =
                MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
            if (format.bytes_per_block && !valid(buffer, format.bytes_per_block)) {
                SkipDraw("invalid texel buffer view");
                return;
            }
        }
    }

    // ---- 2. Allocate every descriptor this draw needs (may submit the command list) ----
    const u32 cbv_size = TableSize(totals.cbv);
    const u32 srv_size = TableSize(totals.srv);
    const u32 uav_size = TableSize(totals.uav);
    descriptors.Reserve(cbv_size + srv_size + uav_size, TableSize(totals.sampler),
                        std::max(num_rts, 1u), has_depth ? 1u : 0u);
    const auto view_block = descriptors.AllocateViews(cbv_size + srv_size + uav_size);
    const auto cbv_table = view_block.Slice(0, cbv_size);
    const auto srv_table = view_block.Slice(cbv_size, srv_size);
    const auto uav_table = view_block.Slice(cbv_size + srv_size, uav_size);
    const auto sampler_table = descriptors.AllocateSamplers(TableSize(totals.sampler));
    const auto rtv_base = descriptors.AllocateRtv(std::max(num_rts, 1u));
    const auto dsv_handle = has_depth ? descriptors.AllocateDsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};

    // ---- 3. Fill the tables and collect resource states ----
    ++feedback_draw;
    bool invalid_states = false;
    bool readonly_depth = false;
    state_scratch.clear();
    const auto need = [this, &invalid_states](const ResourcePtr& resource,
                                              D3D12_RESOURCE_STATES state) {
        if (!resource)
            return;
        for (auto& [known, known_state] : state_scratch) {
            if (known != resource)
                continue;
            constexpr u32 WriteStates = D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                        D3D12_RESOURCE_STATE_RENDER_TARGET |
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (!CompatibleDrawStates(known_state, state, WriteStates))
                invalid_states = true;
            known_state |= state;
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
                auto sampled = view.ImageHandle();
                bool color_feedback = false;
                for (const auto* color : color_views)
                    color_feedback |= color && sampled && color->ImageHandle() == sampled;
                const bool depth_feedback = has_depth && sampled == depth_view->ImageHandle();
                if (color_feedback || depth_feedback) {
                    const bool stencil_writes =
                        regs.stencil_enable != 0 && (regs.stencil_front_mask & 0xff) != 0;
                    if (!color_feedback &&
                        SelectFeedbackMode(depth_feedback, regs.depth_write_enabled != 0,
                                           stencil_writes) == FeedbackMode::ReadOnlyDepth) {
                        readonly_depth = true;
                    } else {
                        sampled = CopyFeedback(sampled);
                        if (!sampled) {
                            SkipDraw("feedback memory budget");
                            return;
                        }
                    }
                }
                if (descriptors.WriteImageSrv(srv_table, srv_i, view, SrvDimension(desc.type),
                                              sampled))
                    need(sampled, SrvState);
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
        const u32 image_start = uav_i;
        uav_i += Shader::NumDescriptors(info->image_descriptors);
        for (u32 i = 0; i < image_buffer_count; ++i, ++uav_i) {
            if (texel_it < bound.texel.size()) {
                const auto& buffer = bound.texel[texel_it++];
                descriptors.WriteTexelUav(uav_table, uav_i, buffer);
                need(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                descriptors.WriteNullUav(uav_table, uav_i);
            }
        }
        const u32 stage_uav_end = uav_i;
        uav_i = image_start;
        for (const auto& desc : info->image_descriptors) {
            const auto format = PixelFormatFromImageFormat(desc.format);
            if (!format) {
                SkipDraw("unsupported graphics storage image format");
                return;
            }
            for (u32 i = 0; i < desc.count; ++i, ++uav_i) {
                auto& view = texture_cache.GetImageView((texture_views++)->id);
                if (const auto resource = view.ImageHandle()) {
                    const auto dimension = view.Description().ViewDimension;
                    if (!(resource->handle->GetDesc().Flags &
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
                        (dimension != D3D12_SRV_DIMENSION_TEXTURE1D &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE1DARRAY &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE2D &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE3D)) {
                        SkipDraw("unsupported graphics storage image view");
                        return;
                    }
                }
                descriptors.WriteImageUav(
                    uav_table, uav_i, view,
                    static_cast<DXGI_FORMAT>(MaxwellFormat(*format).view_format));
                need(view.ImageHandle(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        uav_i = stage_uav_end;
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
    if (!zero_vertex_buffer) {
        zero_vertex_buffer = context.CreateBuffer(65536, D3D12_HEAP_TYPE_UPLOAD);
        std::fill(zero_vertex_buffer->mapped.begin(), zero_vertex_buffer->mapped.end(), u8{0});
    }
    context.Retain(zero_vertex_buffer);
    u32 num_vertex_views = static_cast<u32>(vertex_views.size());
    for (u32 i = 0; i < GeometryBindings::MaxVertexBuffers; ++i) {
        const auto& slot = geometry.vertex[i];
        // Disabled/unmapped guest streams still satisfy the shader signature with zeros.
        if (!regs.vertex_streams[i].enable || !slot.resource || !slot.size) {
            vertex_views[i] = {zero_vertex_buffer->handle->GetGPUVirtualAddress(), 65536, 0};
            continue;
        }
        if (!IsValidBufferView(slot.resource->handle->GetGPUVirtualAddress(),
                               slot.resource->handle->GetDesc().Width, slot.offset, slot.size)) {
            SkipDraw("invalid vertex buffer view");
            return;
        }
        vertex_views[i] = {slot.resource->handle->GetGPUVirtualAddress() + slot.offset,
                           static_cast<UINT>(slot.size), slot.stride};
        need(slot.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    const u32 zero_slot = pipeline->ZeroVertexSlot();
    if (zero_slot < vertex_views.size()) {
        vertex_views[zero_slot] = {zero_vertex_buffer->handle->GetGPUVirtualAddress(), 16, 0};
    }
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const bool quads = draw_state.topology == Maxwell::PrimitiveTopology::Quads ||
                       draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip;
    const bool needs_index = is_indexed || quads || converted_indices.has_value();
    D3D12_INDEX_BUFFER_VIEW index_view{};
    if (converted_indices) {
        index_view = converted_indices->view;
        if (converted_indices->resource)
            need(converted_indices->resource, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    } else if (needs_index) {
        if (!geometry.index || !geometry.index_size || geometry.index_unsupported) {
            if (geometry.index_unsupported && !quad_logged) {
                LOG_WARNING(Render, "D3D12 P3: 8-bit/indexed-quad index conversion not "
                                    "implemented, draw skipped");
                quad_logged = true;
            }
            return;
        }
        if (!IsValidBufferView(geometry.index->handle->GetGPUVirtualAddress(),
                               geometry.index->handle->GetDesc().Width, geometry.index_offset,
                               geometry.index_size)) {
            SkipDraw("invalid index buffer view");
            return;
        }
        index_view = {geometry.index->handle->GetGPUVirtualAddress() + geometry.index_offset,
                      static_cast<UINT>(geometry.index_size),
                      geometry.index_is_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
        need(geometry.index, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
    // Render targets.
    const SIZE_T rtv_stride =
        context.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (u32 slot = 0; slot < num_rts; ++slot) {
        descriptors.CreateRtv({rtv_base.ptr + slot * rtv_stride}, color_views[slot]);
    }
    if (has_depth) {
        auto flags = D3D12_DSV_FLAG_NONE;
        if (readonly_depth) {
            flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
            const auto format = DsvFormat(depth_view->Description().Format);
            if (format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        }
        descriptors.CreateDsv(dsv_handle, *depth_view, flags);
    }
    // Remaining attachment/UAV and storage/geometry aliases cannot have legal
    // combined states. Reject only this draw, leaving the GPU thread alive.
    for (u32 slot = 0; slot < num_rts; ++slot) {
        if (color_views[slot])
            need(color_views[slot]->ImageHandle(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (has_depth)
        need(depth_view->ImageHandle(),
             readonly_depth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (invalid_states) {
        SkipDraw("conflicting resource states");
        return;
    }
    if (readonly_depth)
        ++feedback_readonly_depth;

    // ---- 4. Record state (no more allocations from here on) ----
    auto* const commands = context.Commands();
    descriptors.SetHeaps(commands);
    for (const auto& [resource, state] : state_scratch)
        context.Transition(resource, state);

    commands->SetGraphicsRootSignature(root_signature);
    commands->SetPipelineState(pso);
    commands->IASetPrimitiveTopology(converted_indices ? converted_indices->topology
                                                       : pipeline->Topology());
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
    auto words = pipeline->RootConstants({size.width, size.height, y_flip});
    words[8] = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first;
    words[9] = draw_state.base_instance;
    commands->OMSetStencilRef(regs.stencil_front_ref);
    const float blend[]{regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                        regs.blend_color.a};
    commands->OMSetBlendFactor(blend);
    commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(words.size()), words.data(), 0);

    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              regs.zpass_pixel_count_enable != 0);
    query_runtime.BeginDraw(pipeline->StageInfo(3) != nullptr);
    context.MarkWork("guest draws");
    draw(commands);
    query_runtime.EndDraw();
    if (totals.uav)
        FragmentBarrier();
    for (auto& stage : bindings.stages)
        stage.Clear(); // drop upload-page references so the staging ring can recycle them
}

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) {
    if (BisectEnabled("NXBOX_D3D12_NO_DRAWS")) {
        m_gpu.TickWork();
        return;
    }
    converted_indices.reset();
    const auto& state = maxwell3d->draw_manager.draw_state;
    const auto topology = static_cast<IndexConversion::Topology>(state.topology);
    if (is_indexed &&
        static_cast<u32>(topology) <= static_cast<u32>(IndexConversion::Topology::Polygon)) {
        gpu_memory->FlushCaching();
        const auto format =
            static_cast<IndexConversion::Format>(maxwell3d->regs.index_buffer.format);
        const size_t width = size_t{1} << static_cast<u32>(format);
        std::vector<std::byte> input(size_t{state.index_buffer.count} * width);
        gpu_memory->ReadBlock(maxwell3d->regs.index_buffer.StartAddress() +
                                  size_t{state.index_buffer.first} * width,
                              input.data(), input.size());
        converted_indices = index_converter.Convert(topology, format, input,
                                                    {maxwell3d->regs.primitive_restart.enabled != 0,
                                                     maxwell3d->regs.primitive_restart.index});
    } else if (topology == IndexConversion::Topology::Quads ||
               topology == IndexConversion::Topology::QuadStrip ||
               topology == IndexConversion::Topology::TriangleFan ||
               topology == IndexConversion::Topology::Polygon ||
               topology == IndexConversion::Topology::LineLoop) {
        converted_indices = index_converter.Generate(topology, state.vertex_buffer.count);
    }
    if (converted_indices && !converted_indices->count) {
        converted_indices.reset();
        m_gpu.TickWork();
        return;
    }
    PrepareDraw(is_indexed, [&](ID3D12GraphicsCommandList* commands) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        DrawParams params = MakeDrawParams(draw_state, instance_count, is_indexed);
        if (converted_indices) {
            params.num_vertices = converted_indices->count;
            params.first_index = 0;
            params.base_vertex =
                is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first;
            params.is_indexed = true;
        }
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
    converted_indices.reset();
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
              "CreateCommandSignature", context.Device());
    }
    return signature.Get();
}

void RasterizerD3D12::DrawIndirect() {
    if (BisectEnabled("NXBOX_D3D12_NO_DRAWS")) {
        m_gpu.TickWork();
        return;
    }
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

bool RasterizerD3D12::AccelerateConditionalRendering() {
    gpu_memory->FlushCaching();
    return query_cache.AccelerateHostConditionalRendering();
}
void RasterizerD3D12::PreparePresentation() {
    query_runtime.PauseHostConditionalRendering();
}
void RasterizerD3D12::DrawTexture() {}
void RasterizerD3D12::Clear(u32 layer_count) {
    gpu_memory->FlushCaching();
    query_runtime.PauseHostConditionalRendering();
    std::scoped_lock lock{texture_cache.mutex};
    const auto& regs = maxwell3d->regs;
    u32 control{};
    std::memcpy(&control, &regs.clear_control, sizeof(control));
    u32 surface{};
    std::memcpy(&surface, &regs.clear_surface, sizeof(surface));
    const auto selection = ClearOperations::Decode(surface, control, regs.stencil_front_mask);
    texture_cache.UpdateRenderTargets(true);
    auto* framebuffer = texture_cache.GetFramebuffer();
    if (!framebuffer)
        return;
    if (selection.target >= VideoCommon::NUM_RT)
        return;
    auto* color = framebuffer->Color(selection.target);
    auto* depth = framebuffer->Depth();
    ClearValues values;
    values.color = regs.clear_color;
    values.depth = regs.clear_depth;
    values.stencil = static_cast<u8>(regs.clear_stencil);
    if (color) {
        const auto pixel = color->format;
        if (VideoCore::Surface::IsPixelFormatInteger(pixel)) {
            values.color_type = VideoCore::Surface::IsPixelFormatSignedInteger(pixel)
                                    ? ClearColorType::Sint
                                    : ClearColorType::Uint;
            for (size_t i = 0; i < 4; ++i) {
                values.integer_color[i] = std::bit_cast<u32>(regs.clear_color[i]);
            }
        }
    }
    const auto size = framebuffer->Size();
    ClearOperations::Rect clip{0, 0, static_cast<s32>(size.width), static_cast<s32>(size.height)};
    if (regs.clear_control.use_scissor && regs.scissor_test[0].enable) {
        const auto& scissor = regs.scissor_test[0];
        clip = ClearOperations::Intersect(
            clip, {static_cast<s32>(scissor.min_x), static_cast<s32>(scissor.min_y),
                   static_cast<s32>(scissor.max_x), static_cast<s32>(scissor.max_y)});
    }
    if (regs.clear_control.use_clear_rect) {
        clip = ClearOperations::Intersect(clip, {static_cast<s32>(regs.clear_rect.x_min),
                                                 static_cast<s32>(regs.clear_rect.y_min),
                                                 static_cast<s32>(regs.clear_rect.x_max),
                                                 static_cast<s32>(regs.clear_rect.y_max)});
    }
    if (regs.clear_control.use_viewport_clip0) {
        const auto& viewport = regs.viewports[0];
        clip = ClearOperations::Intersect(clip, {static_cast<s32>(viewport.x),
                                                 static_cast<s32>(viewport.y),
                                                 static_cast<s32>(viewport.x + viewport.width),
                                                 static_cast<s32>(viewport.y + viewport.height)});
    }
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        const auto top = clip.top;
        clip.top = static_cast<s32>(size.height) - clip.bottom;
        clip.bottom = static_cast<s32>(size.height) - top;
    }
    for (u32 layer = selection.layer; layer < selection.layer + layer_count; ++layer) {
        ClearAttachment color_attachment, depth_attachment;
        descriptors.Reserve(0, 0, selection.color_mask ? 1u : 0u,
                            (selection.depth || selection.stencil) ? 1u : 0u);
        const auto prepare = [&](ImageView* view, bool is_depth, ClearAttachment& attachment) {
            if (!view || !view->ImageHandle())
                return false;
            const auto& srv = view->Description();
            const u32 first = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                  ? srv.Texture2DArray.FirstArraySlice
                                  : 0;
            const u32 layers = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                   ? srv.Texture2DArray.ArraySize
                                   : 1;
            if (layer >= layers)
                return false;
            const u32 mip = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                ? srv.Texture2DArray.MostDetailedMip
                                : srv.Texture2D.MostDetailedMip;
            attachment.resource = view->ImageHandle();
            attachment.width =
                static_cast<u32>(attachment.resource->handle->GetDesc().Width >> mip);
            attachment.height = attachment.resource->handle->GetDesc().Height >> mip;
            attachment.samples = attachment.resource->handle->GetDesc().SampleDesc;
            if (is_depth) {
                attachment.descriptor = descriptors.AllocateDsv();
                attachment.format = DsvFormat(srv.Format);
                attachment.has_depth = true;
                attachment.has_stencil = attachment.format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                                         attachment.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
                desc.Format = attachment.format;
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray = {mip, first + layer, 1};
                context.Device()->CreateDepthStencilView(attachment.resource->handle.Get(), &desc,
                                                         attachment.descriptor);
            } else {
                attachment.descriptor = descriptors.AllocateRtv(1);
                attachment.format = srv.Format;
                D3D12_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = srv.Format;
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray = {mip, first + layer, 1, 0};
                context.Device()->CreateRenderTargetView(attachment.resource->handle.Get(), &desc,
                                                         attachment.descriptor);
            }
            return true;
        };
        const bool has_color = selection.color_mask && prepare(color, false, color_attachment);
        const bool has_depth =
            (selection.depth || selection.stencil) && prepare(depth, true, depth_attachment);
        clear_renderer.Execute(selection, values, has_color ? &color_attachment : nullptr,
                               has_depth ? &depth_attachment : nullptr, &clip);
    }
    m_gpu.TickWork();
}
void RasterizerD3D12::DispatchCompute() {
    if (BisectEnabled("NXBOX_D3D12_NO_COMPUTE")) {
        m_gpu.TickWork();
        return;
    }
    gpu_memory->FlushCaching();
    query_runtime.PauseHostConditionalRendering();
    auto* pipeline = default_pipeline_cache->CurrentComputePipeline();
    if (!pipeline)
        return;
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto& info = pipeline->info;
    const auto& qmd = kepler_compute->launch_description;
    texture_cache.SynchronizeDescriptors(true);
    bindings.current_stage = 0;
    bindings.stages[0].Clear();
    std::copy_n(info.constant_buffer_used_sizes.begin(), compute_uniform_sizes.size(),
                compute_uniform_sizes.begin());
    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &compute_uniform_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    size_t storage_index = 0;
    for (const auto& desc : info.storage_buffers_descriptors)
        buffer_cache.BindComputeStorageBuffer(storage_index++, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written);
    view_scratch.clear();
    sampler_scratch.clear();
    const auto handle = [&](const auto& desc, u32 index) {
        const u32 offset = index << desc.size_shift;
        u32 value = gpu_memory->Read<u32>(qmd.const_buffer_config[desc.cbuf_index].Address() +
                                          desc.cbuf_offset + offset);
        if constexpr (requires { desc.has_secondary; }) {
            value <<= desc.shift_left;
            if (desc.has_secondary)
                value |= gpu_memory->Read<u32>(
                             qmd.const_buffer_config[desc.secondary_cbuf_index].Address() +
                             desc.secondary_cbuf_offset + offset)
                         << desc.secondary_shift_left;
        }
        return Tegra::Texture::TexturePair(value, qmd.linked_tsc != 0);
    };
    const auto gather = [&](const auto& descs, bool written) {
        for (const auto& desc : descs)
            for (u32 i = 0; i < desc.count; ++i)
                view_scratch.push_back(
                    {.index = handle(desc, i).first, .blacklist = written, .id = {}});
    };
    gather(info.texture_buffer_descriptors, false);
    gather(info.image_buffer_descriptors, true);
    gather(info.texture_descriptors, false);
    gather(info.image_descriptors, true);
    for (const auto& desc : info.texture_descriptors)
        for (u32 i = 0; i < desc.count; ++i)
            sampler_scratch.push_back(texture_cache.GetSamplerId(handle(desc, i).second, true));
    texture_cache.FillImageViews(std::span(view_scratch.data(), view_scratch.size()), true,
                                 !info.image_descriptors.empty());
    buffer_cache.UnbindComputeTextureBuffers();
    size_t texel_index = 0, view_index = 0;
    for (const auto& desc : info.texture_buffer_descriptors)
        for (u32 i = 0; i < desc.count; ++i) {
            auto& view = texture_cache.GetImageView(view_scratch[view_index++].id);
            buffer_cache.BindComputeTextureBuffer(texel_index++, view.GpuAddr(), view.BufferSize(),
                                                  view.format, false, false);
        }
    for (const auto& desc : info.image_buffer_descriptors)
        for (u32 i = 0; i < desc.count; ++i) {
            auto& view = texture_cache.GetImageView(view_scratch[view_index++].id);
            const auto format = PixelFormatFromImageFormat(desc.format);
            if (!format)
                throw std::runtime_error("D3D12 unsupported compute image buffer format");
            buffer_cache.BindComputeTextureBuffer(texel_index++, view.GpuAddr(), view.BufferSize(),
                                                  *format, desc.is_written, true);
        }
    buffer_cache.UpdateComputeBuffers();
    buffer_cache.BindHostComputeBuffers();
    const auto counts = CountStageTables(info);
    const u32 cbv_size = TableSize(counts.cbv), srv_size = TableSize(counts.srv),
              uav_size = TableSize(counts.uav);
    descriptors.Reserve(cbv_size + srv_size + uav_size, TableSize(counts.sampler));
    const auto allocation = descriptors.AllocateViews(cbv_size + srv_size + uav_size);
    const auto cbv = allocation.Slice(0, cbv_size), srv = allocation.Slice(cbv_size, srv_size),
               uav = allocation.Slice(cbv_size + srv_size, uav_size);
    const auto sampler = descriptors.AllocateSamplers(TableSize(counts.sampler));
    std::vector<ComputeResourceUse> resources;
    const auto use = [&](const ResourcePtr& resource, D3D12_RESOURCE_STATES state) {
        if (resource)
            resources.push_back({resource, state});
    };
    const auto& bound = bindings.stages[0];
    for (u32 i = 0; i < cbv_size; ++i) {
        if (i < bound.uniform.size()) {
            descriptors.WriteCbv(cbv, i, bound.uniform[i]);
            use(bound.uniform[i].resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        } else
            descriptors.WriteNullCbv(cbv, i);
    }
    u32 srv_index = 0, sampler_index = 0;
    for (const auto& desc : info.texture_descriptors)
        for (u32 i = 0; i < desc.count; ++i) {
            auto& view = texture_cache.GetImageView(view_scratch[view_index++].id);
            descriptors.WriteImageSrv(srv, srv_index++, view, SrvDimension(desc.type));
            use(view.ImageHandle(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            descriptors.WriteSampler(sampler, sampler_index,
                                     texture_cache.GetSampler(sampler_scratch[sampler_index]));
            ++sampler_index;
        }
    const u32 texel_count = Shader::NumDescriptors(info.texture_buffer_descriptors);
    for (u32 i = 0; i < texel_count; ++i) {
        descriptors.WriteTexelSrv(srv, srv_index++, bound.texel[i]);
        use(bound.texel[i].resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (; srv_index < srv_size; ++srv_index)
        descriptors.WriteNullSrv(srv, srv_index, D3D12_SRV_DIMENSION_TEXTURE2D);
    for (; sampler_index < sampler.count; ++sampler_index)
        descriptors.WriteNullSampler(sampler, sampler_index);
    u32 uav_index = 0;
    for (const auto& buffer : bound.storage) {
        descriptors.WriteStorageUav(uav, uav_index++, buffer);
        use(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const u32 image_start = uav_index;
    uav_index += Shader::NumDescriptors(info.image_descriptors);
    for (size_t i = texel_count; i < bound.texel.size(); ++i) {
        descriptors.WriteTexelUav(uav, uav_index++, bound.texel[i]);
        use(bound.texel[i].resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const u32 uav_end = uav_index;
    uav_index = image_start;
    for (const auto& desc : info.image_descriptors)
        for (u32 i = 0; i < desc.count; ++i) {
            auto& view = texture_cache.GetImageView(view_scratch[view_index++].id);
            const auto format = PixelFormatFromImageFormat(desc.format);
            if (!format)
                throw std::runtime_error("D3D12 unsupported compute storage image format");
            descriptors.WriteImageUav(uav, uav_index++, view,
                                      static_cast<DXGI_FORMAT>(MaxwellFormat(*format).view_format));
            use(view.ImageHandle(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    uav_index = uav_end;
    for (; uav_index < uav_size; ++uav_index)
        descriptors.WriteNullUav(uav, uav_index);
    ComputeBindings dispatch;
    dispatch.pipeline = pipeline->PipelineState();
    dispatch.root_signature = pipeline->RootSignature();
    dispatch.resource_heap = descriptors.ResourceHeap();
    dispatch.sampler_heap = descriptors.SamplerHeap();
    dispatch.tables = {cbv.gpu, srv.gpu, uav.gpu, sampler.gpu};
    Shader::Backend::HLSL::RootConstants constants;
    std::memcpy(dispatch.constants.data(), &constants, sizeof(constants));
    dispatch.resources = resources;
    context.MarkWork("guest compute");
    compute_dispatch.Direct(dispatch, {qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z});
    bindings.stages[0].Clear();
    m_gpu.TickWork();
}
void RasterizerD3D12::ResetCounter(VideoCommon::QueryType type) {
    query_cache.CounterReset(type);
}
void RasterizerD3D12::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
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
    fence_manager.SignalReference();
    fence_manager.WaitPendingFences(true);
    context.Finish();
}
void RasterizerD3D12::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (!addr || !size)
        return;
    if (True(which & VideoCommon::CacheType::QueryCache))
        query_cache.FlushRegion(addr, size);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    if (True(which & VideoCommon::CacheType::BufferCache))
        buffer_cache.DownloadMemory(addr, size);
    if (True(which & VideoCommon::CacheType::TextureCache))
        texture_cache.DownloadMemory(addr, size);
}
bool RasterizerD3D12::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    return (True(which & VideoCommon::CacheType::QueryCache) &&
            query_cache.IsRegionGpuModified(addr, size)) ||
           (True(which & VideoCommon::CacheType::BufferCache) &&
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
    if (True(which & VideoCommon::CacheType::ShaderCache))
        default_pipeline_cache->InvalidateRegion(addr, size);
    if (True(which & VideoCommon::CacheType::QueryCache))
        query_cache.InvalidateRegion(addr, size);
}
bool RasterizerD3D12::OnCPUWrite(DAddr addr, u64 size) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    default_pipeline_cache->OnCacheInvalidation(addr, size);
    query_cache.InvalidateRegion(addr, size);
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
    default_pipeline_cache->InvalidateRegion(addr, size);
    query_cache.InvalidateRegion(addr, size);
    texture_cache.UnmapMemory(addr, size);
    buffer_cache.WriteMemory(addr, size);
}
void RasterizerD3D12::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    texture_cache.UnmapGPUMemory(as_id, addr, size);
    buffer_cache.UnmapGPUMemory(as_id, addr, size);
}
void RasterizerD3D12::SignalFence(std::function<void()>&& func) {
    fence_manager.SignalFence(std::move(func));
}
void RasterizerD3D12::SyncOperation(std::function<void()>&& func) {
    fence_manager.SyncOperation(std::move(func));
}
void RasterizerD3D12::SignalSyncPoint(u32 value) {
    fence_manager.SignalSyncPoint(value);
}
void RasterizerD3D12::SignalReference() {
    fence_manager.SignalReference();
}
void RasterizerD3D12::ReleaseFences(bool force) {
    fence_manager.WaitPendingFences(force);
}
void RasterizerD3D12::FlushAndInvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    FlushRegion(addr, size, which);
    InvalidateRegion(addr, size, which);
}
void RasterizerD3D12::WaitForIdle() {
    query_cache.NotifyWFI();
    context.Finish();
    fence_manager.SignalOrdering();
}
void RasterizerD3D12::FragmentBarrier() {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    context.Commands()->ResourceBarrier(1, &barrier);
}
void RasterizerD3D12::TiledCacheBarrier() {
    FragmentBarrier();
}
void RasterizerD3D12::FlushCommands() {
    // Cache batches and guest callbacks belong to FenceManager, never submission.
    context.Submit();
    default_pipeline_cache->FlushDiskCache();
}
void RasterizerD3D12::TickFrame() {
    fence_manager.TickFrame();
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.TickFrame();
    texture_cache.TickFrame();
    if (feedback_frame < 8 || feedback_frame % 120 == 0) {
        Diagnostic("D3D12_FEEDBACK copies=" + std::to_string(feedback_copies) +
                   " readonly_depth=" + std::to_string(feedback_readonly_depth) +
                   " skipped_draws=" + std::to_string(skipped_draws) +
                   " frame=" + std::to_string(feedback_frame));
    }
    feedback_copies = feedback_readonly_depth = skipped_draws = 0;
    std::erase_if(feedback_pool,
                  [this](const auto& entry) { return entry.frame != feedback_frame; });
    ++feedback_frame;
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
                                        const VideoCore::DiskResourceLoadCallback& callback) {
    default_pipeline_cache->LoadDiskResources(title_id, stop_loading, callback);
}
void RasterizerD3D12::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.CreateChannel(channel);
    texture_cache.CreateChannel(channel);
    default_pipeline_cache->CreateChannel(channel);
    query_cache.CreateChannel(channel);
}
void RasterizerD3D12::BindChannel(Tegra::Control::ChannelState& channel) {
    BindToChannel(channel.bind_id);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.BindToChannel(channel.bind_id);
    texture_cache.BindToChannel(channel.bind_id);
    default_pipeline_cache->BindToChannel(channel.bind_id);
    query_cache.BindToChannel(channel.bind_id);
    query_runtime.Bind3DEngine(maxwell3d);
}
void RasterizerD3D12::ReleaseChannel(s32 channel_id) {
    EraseChannel(channel_id);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.EraseChannel(channel_id);
    texture_cache.EraseChannel(channel_id);
    default_pipeline_cache->EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
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
