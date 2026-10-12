// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <fmt/format.h>
#include "common/alignment.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "shader_recompiler/backend/hlsl/hlsl_draw_texture.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_buffer_view.h"
#include "video_core/renderer_d3d12/d3d12_feedback.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_render_area.h"
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
    context.Profile().guest_visibility = [this] {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        return query_runtime.VisibilityDiagnostics() +
               " pending_guest_fences=" + std::to_string(fence_manager.PendingGuestFences()) +
               " pending_guest_callbacks=" + std::to_string(fence_manager.PendingGuestCallbacks()) +
               " uncommitted_buffer_writes=" +
               std::to_string(buffer_cache.HasUncommittedFlushes()) +
               " uncommitted_texture_writes=" +
               std::to_string(texture_cache.HasUncommittedFlushes());
    };
}
RasterizerD3D12::~RasterizerD3D12() {
    context.Profile().guest_visibility = {};
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
// CPU expansion preserves the guest multi-draw ID independently of base IDs.
// Scoped to the GPU thread and restored after each indirect batch.
thread_local u32 indirect_draw_id{};

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

// Keep the submitted draw parameters available when sizing vertex bindings.
struct DrawCommand {
    DrawParams params;
    void operator()(ID3D12GraphicsCommandList* commands) const {
        if (!params.num_vertices || !params.num_instances)
            return;
        if (params.is_indexed)
            commands->DrawIndexedInstanced(params.num_vertices, params.num_instances,
                                           params.first_index, static_cast<INT>(params.base_vertex),
                                           params.base_instance);
        else
            commands->DrawInstanced(params.num_vertices, params.num_instances, params.base_vertex,
                                    params.base_instance);
    }
};

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
    const auto& src = regs.scissor_test[index];
    ClearOperations::Rect clip{0, 0, 16384, 16384};
    if (!regs.viewport_scale_offset_enabled) {
        clip = {static_cast<s32>(regs.surface_clip.x), static_cast<s32>(regs.surface_clip.y),
                static_cast<s32>(regs.surface_clip.x + regs.surface_clip.width),
                static_cast<s32>(regs.surface_clip.y + regs.surface_clip.height)};
    }
    if (src.enable) {
        clip = ClearOperations::Intersect(
            clip, {static_cast<s32>(src.min_x), static_cast<s32>(src.min_y),
                   static_cast<s32>(src.max_x), static_cast<s32>(src.max_y)});
    }
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        const auto top = clip.top;
        clip.top = static_cast<s32>(regs.surface_clip.height) - clip.bottom;
        clip.bottom = static_cast<s32>(regs.surface_clip.height) - top;
    }
    clip.left = (std::max)(clip.left, 0);
    clip.top = (std::max)(clip.top, 0);
    return {clip.left, clip.top, (std::max)(clip.left, clip.right),
            (std::max)(clip.top, clip.bottom)};
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

D3D12_SRV_DIMENSION SrvDimension(Shader::TextureType type, bool multisample = false) {
    if (multisample)
        return type == Shader::TextureType::ColorArray2D ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                         : D3D12_SRV_DIMENSION_TEXTURE2DMS;
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
    // Descriptor slots and recorded copies belong to CurrentTick even if no draw
    // reaches OMSetRenderTargets. Submit that producer so they can retire.
    // Transitions are immediate; this backend never opens a native render pass.
    for (auto& stage : bindings.stages)
        stage.Clear();
    state_scratch.clear();
    converted_indices.reset();
    try {
        query_runtime.CancelDraw();
        context.Submit();
    } catch (const std::exception& error) {
        context.ReportFailure(error);
    }
    ++skipped_draws;
    static std::mutex warning_mutex;
    static std::set<std::string> warnings;
    {
        std::scoped_lock lock{warning_mutex};
        if (warnings.emplace(reason).second) {
            Diagnostic(std::string{"D3D12_DRAW_SKIP reason="} + reason);
            LOG_WARNING(Render, "D3D12 draw fallback: {}", reason);
        }
    }
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
        u64 pool_bytes{};
        for (const auto& entry : feedback_pool)
            pool_bytes += entry.resource->allocation_size;
        // A cache target, not a draw limit. Evicted resources remain owned by
        // Context's submission batches until their fences complete. Copies reused
        // on the same queue are ordered after earlier sampling by Transition.
        constexpr u64 PoolTarget = 64ULL << 20;
        std::erase_if(feedback_pool, [this, pool_bytes](const auto& entry) {
            return entry.draw != feedback_draw &&
                   (entry.frame != feedback_frame || pool_bytes >= PoolTarget);
        });
        pool_bytes = 0;
        for (const auto& entry : feedback_pool)
            pool_bytes += entry.resource->allocation_size;
        const auto allocation = context.Device()->GetResourceAllocationInfo(0, 1, &desc);
        try {
            feedback_pool.push_back({context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT,
                                                            D3D12_RESOURCE_STATE_COPY_DEST)});
        } catch (const std::exception& error) {
            if (!feedback_failure_logged) {
                feedback_failure_logged = true;
                Diagnostic(fmt::format("D3D12_FEEDBACK_ALLOCATION_FAILED width={} height={} "
                                       "layers={} mips={} bytes={} pool_bytes={} memory_bytes={} "
                                       "reason={}",
                                       desc.Width, desc.Height, desc.DepthOrArraySize,
                                       desc.MipLevels, allocation.SizeInBytes, pool_bytes,
                                       context.MemoryUsage(), error.what()));
            }
            return {};
        }
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
    SCOPE_EXIT {
        for (auto& stage : bindings.stages)
            stage.Clear();
        state_scratch.clear();
    };
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
    u32 attachment_samples = 0;
    const auto accept_attachment = [&](const ImageView& view, bool depth) {
        const auto resource = view.ImageHandle();
        if (!resource)
            return false;
        const auto desc = resource->handle->GetDesc();
        const auto required = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                    : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!(desc.Flags & required)) {
            view.LogUnsupported(depth ? "depth attachment lacks ALLOW_DEPTH_STENCIL"
                                      : "color attachment lacks ALLOW_RENDER_TARGET");
            return false;
        }
        if (desc.SampleDesc.Quality != 0 ||
            (attachment_samples && attachment_samples != desc.SampleDesc.Count)) {
            view.LogUnsupported(
                fmt::format("attachment sample mismatch: bound={} count={} quality={}",
                            attachment_samples, desc.SampleDesc.Count, desc.SampleDesc.Quality));
            return false;
        }
        attachment_samples = desc.SampleDesc.Count;
        formats.sample_count = static_cast<u8>(attachment_samples);
        return true;
    };
    std::array<ImageView*, VideoCommon::NUM_RT> color_views{};
    const u32 guest_rts = std::min<u32>(regs.rt_control.count, VideoCommon::NUM_RT);
    for (u32 slot = 0; slot < guest_rts; ++slot) {
        auto* view = framebuffer->Color(framebuffer->DrawBuffer(slot));
        if (view && !accept_attachment(*view, false)) {
            SkipDraw("unsupported attachment or mismatched sample counts");
            return;
        }
        if (view && view->ImageHandle()) {
            // Only overlapping RTV subresources conflict. Distinct mips/layers
            // of one resource remain legal independent color attachments.
            bool duplicate = false;
            const auto resource_desc = view->ImageHandle()->handle->GetDesc();
            for (u32 previous = 0; previous < slot; ++previous) {
                const auto* prior = color_views[previous];
                if (!prior || prior->ImageHandle() != view->ImageHandle())
                    continue;
                const bool same_mip = resource_desc.SampleDesc.Count > 1 ||
                                      prior->range.base.level == view->range.base.level;
                duplicate |=
                    same_mip &&
                    (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
                     Pso::RenderTargetLayersOverlap(static_cast<u32>(prior->range.base.layer),
                                                    static_cast<u32>(prior->range.extent.layers),
                                                    static_cast<u32>(view->range.base.layer),
                                                    static_cast<u32>(view->range.extent.layers)));
            }
            if (!duplicate) {
                color_views[slot] = view;
                formats.colors[slot] = static_cast<u32>(view->Description().Format);
            }
        }
    }
    ImageView* const depth_view = framebuffer->Depth();
    if (depth_view && !accept_attachment(*depth_view, true)) {
        SkipDraw("unsupported depth attachment or mismatched sample counts");
        return;
    }
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
    FillGraphicsState(*maxwell3d, attachment_state);
    Pso::ApplyFramebufferFormats(attachment_state, formats);
    if (attachment_diagnostics < 8) {
        ++attachment_diagnostics;
        std::string message = "D3D12_ATTACHMENTS count=" + std::to_string(guest_rts);
        const auto* fragment = pipeline->StageInfo(4);
        for (u32 slot = 0; slot < VideoCommon::NUM_RT; ++slot) {
            message += " slot" + std::to_string(slot) + "=" + std::to_string(formats.colors[slot]);
            if (fragment && fragment->stores_frag_color[slot] && !color_views[slot])
                message += "(unbound_shader_output)";
        }
        message += " depth=" + std::to_string(formats.depth);
        Diagnostic(message);
    }

    ++feedback_draw;
    const auto extent = [](const ImageView& view) {
        const auto desc = view.ImageHandle()->handle->GetDesc();
        const u32 mip = desc.SampleDesc.Count > 1 ? 0 : static_cast<u32>(view.range.base.level);
        return AttachmentExtent{static_cast<u32>(std::max<u64>(1, desc.Width >> mip)),
                                std::max(1u, desc.Height >> mip),
                                static_cast<u32>(view.range.extent.layers)};
    };
    AttachmentExtent render_area{16384, 16384, 2048};
    bool has_attachment = false;
    for (const auto* color : color_views) {
        if (color) {
            render_area = IntersectAttachments(render_area, extent(*color));
            has_attachment = true;
        }
    }
    if (has_depth) {
        render_area = IntersectAttachments(render_area, extent(*depth_view));
        has_attachment = true;
    }
    if (!has_attachment) {
        const auto size = framebuffer->Size();
        render_area = {size.width, size.height, 1};
    }
    if (!render_area.width || !render_area.height || !render_area.layers)
        return;
    D3D12_RESOURCE_DIMENSION attachment_dimension =
        has_depth ? depth_view->ImageHandle()->handle->GetDesc().Dimension
                  : D3D12_RESOURCE_DIMENSION_UNKNOWN;
    for (const auto* color : color_views) {
        if (!color)
            continue;
        const auto dimension = color->ImageHandle()->handle->GetDesc().Dimension;
        if (attachment_dimension != D3D12_RESOURCE_DIMENSION_UNKNOWN &&
            dimension != attachment_dimension) {
            SkipDraw("mismatched attachment resource types");
            return;
        }
        attachment_dimension = dimension;
    }
    const auto matching_extent = MatchingAttachmentExtent(
        render_area, has_depth ? extent(*depth_view) : render_area, has_depth);
    std::array<ResourcePtr, VideoCommon::NUM_RT> color_resources{};
    struct ColorCopy {
        BlitSurface original;
        BlitSurface temporary;
        VideoCommon::ImageCopy region;
    };
    std::vector<ColorCopy> color_copies;
    for (u32 slot = 0; slot < num_rts; ++slot) {
        const auto* color = color_views[slot];
        if (!color)
            continue;
        color_resources[slot] = color->ImageHandle();
        const auto original_extent = extent(*color);
        if (original_extent.width == matching_extent.width &&
            original_extent.height == matching_extent.height)
            continue;
        auto desc = color_resources[slot]->handle->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            SkipDraw("mismatched non-2D attachment extents");
            return;
        }
        desc.Width = matching_extent.width;
        desc.Height = matching_extent.height;
        desc.DepthOrArraySize = static_cast<UINT16>(matching_extent.layers);
        desc.MipLevels = 1;
        desc.Alignment = 0;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        ResourcePtr temporary;
        for (auto& entry : attachment_pool) {
            const auto cached = entry.resource->handle->GetDesc();
            if (entry.draw != feedback_draw && cached.Width == desc.Width &&
                cached.Height == desc.Height && cached.DepthOrArraySize == desc.DepthOrArraySize &&
                cached.Format == desc.Format && cached.Flags == desc.Flags &&
                cached.SampleDesc.Count == desc.SampleDesc.Count &&
                cached.SampleDesc.Quality == desc.SampleDesc.Quality) {
                entry.draw = feedback_draw;
                entry.frame = feedback_frame;
                temporary = entry.resource;
                break;
            }
        }
        if (!temporary) {
            temporary = context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_STATE_COPY_DEST);
            attachment_pool.push_back({temporary, nullptr, feedback_draw, feedback_frame});
        }
        color_resources[slot] = temporary;
        const VideoCommon::ImageCopy copy{
            .src_subresource = {color->range.base.level, color->range.base.layer,
                                static_cast<s32>(render_area.layers)},
            .dst_subresource = {0, 0, static_cast<s32>(render_area.layers)},
            .src_offset = {0, 0, 0},
            .dst_offset = {0, 0, 0},
            .extent = {render_area.width, render_area.height, 1},
        };
        const BlitSurface original{color->ImageHandle(), color->HostFormat()};
        const BlitSurface target{temporary, color->HostFormat()};
        // CopyImage uses a bitwise region copy for single-sample color and a
        // sample-preserving shader copy for MSAA (never an averaging resolve).
        texture_runtime.BlitHelpers().CopyImage(target, original, std::span{&copy, 1});
        color_copies.push_back({original, target, copy});
    }
    if (!color_copies.empty() && attachment_extent_diagnostics++ < 16) {
        Diagnostic(fmt::format("D3D12_ATTACHMENT_EXTENTS area={}x{} layers={} matched={}x{} "
                               "color_temporaries={}",
                               render_area.width, render_area.height, render_area.layers,
                               matching_extent.width, matching_extent.height, color_copies.size()));
    }

    // ---- 1. Gather views/samplers and tell the buffer cache what the shaders use ----
    StageTableCounts totals{};
    bool has_images = false;
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        uniform_masks[stage] = 0;
        uniform_sizes[stage].fill(0);
        if (const Shader::Info* info = pipeline->StageInfo(stage)) {
            uniform_masks[stage] = info->constant_buffer_mask;
            // The HLSL byte-addressed cbuffer can read dynamic offsets. Bind the
            // full guest range (the shared cache clamps this to the guest size).
            uniform_sizes[stage].fill(65536);
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
                    view_scratch.push_back(
                        {.index = read_handle(desc, i).first, .blacklist = blacklist, .id = {}});
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

    // Resource creation can drain the queue under memory pressure. Do it before
    // reserving descriptor slots, whose lifetime is tied to the recording tick.
    for (const auto& item : view_scratch) {
        const auto sampled = texture_cache.GetImageView(item.id).ImageHandle();
        if (!sampled)
            continue;
        bool feedback = false;
        for (const auto& color : color_resources)
            feedback |= color == sampled;
        const bool depth_feedback = has_depth && depth_view->ImageHandle() == sampled;
        const bool stencil_writes =
            regs.stencil_enable &&
            ((regs.stencil_front_mask & 0xff) ||
             (regs.stencil_two_side_enable && (regs.stencil_back_mask & 0xff)));
        feedback |= depth_feedback && SelectFeedbackMode(true, regs.depth_write_enabled != 0,
                                                         stencil_writes) == FeedbackMode::Copy;
        if (feedback && !CopyFeedback(sampled)) {
            SkipDraw("feedback allocation failed");
            return;
        }
    }

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
            constexpr u32 WriteStates =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET |
                D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT;
            if (!CompatibleDrawStates(known_state, state, WriteStates))
                invalid_states = true;
            known_state |= state;
            return;
        }
        state_scratch.emplace_back(resource, state);
    };
    // Geometry buffers.
    auto& geometry = bindings.geometry;
    std::array<D3D12_VERTEX_BUFFER_VIEW, GeometryBindings::MaxVertexBuffers> vertex_views{};
    if (!zero_vertex_buffer) {
        zero_vertex_buffer = context.CreateBuffer(65536, D3D12_HEAP_TYPE_UPLOAD);
        std::fill(zero_vertex_buffer->mapped.begin(), zero_vertex_buffer->mapped.end(), u8{0});
    }
    context.Retain(zero_vertex_buffer);
    u32 num_vertex_views = static_cast<u32>(vertex_views.size());
    const GraphicsPipeline* const native_pipeline = pipeline->AsNative();
    if (!native_pipeline) {
        SkipDraw("vertex layout unavailable for native pipeline");
        return;
    }
    const auto& submitted = draw.params;
    if (!submitted.num_vertices || !submitted.num_instances)
        return;
    const auto& guest_draw = maxwell3d->draw_manager.draw_state;
    // Index expansion increases submitted index count, not the maximum guest
    // vertex. Use the original vertex range for generated fans/strips/quads.
    u64 last_vertex =
        Pso::VertexLastNonIndexed(submitted.base_vertex, guest_draw.vertex_buffer.count);
    // Indexed draws can reference any vertex in the bound stream. Reading and scanning the
    // guest index buffer on the CPU per draw stalled the GPU thread for seconds (it forces
    // GPU memory reads), so bind the whole stream range the guest declared instead, like the
    // Vulkan backend does.
    const bool whole_stream = is_indexed;
    for (u32 i = 0; i < Pso::kNumVertexAttributes; ++i) {
        const auto& attribute = native_pipeline->State().attributes[i];
        vertex_views[i] = {zero_vertex_buffer->handle->GetGPUVirtualAddress(), 65536, 0};
        if (!attribute.enabled || attribute.enabled == 2)
            continue;
        const auto& slot = geometry.vertex[attribute.slot];
        if (!regs.vertex_streams[attribute.slot].enable || !slot.resource || !slot.size)
            continue;
        const auto& input = regs.vertex_attrib_format[i];
        const u32 vertex_size = static_cast<u32>(input.size.Value());
        const u32 bytes = Pso::VertexByteSize(vertex_size);
        const u32 storage_bytes = Pso::VertexStorageByteSize(vertex_size, attribute.swap_rb != 0);
        const u32 alignment = Pso::VertexAlignment(static_cast<u32>(input.size.Value()));
        u64 last = attribute.instanced
                             ? (attribute.divisor == 0 ? 0
                                                       : u64{submitted.base_instance} +
                                                             (submitted.num_instances - 1) /
                                                                 std::max(attribute.divisor, 1u))
                             : last_vertex;
        const u64 stream_available =
            slot.size > attribute.offset ? slot.size - attribute.offset : 0;
        if (whole_stream && !attribute.instanced)
            last = slot.stride && stream_available >= bytes
                       ? (stream_available - bytes) / slot.stride
                       : 0;
        const u64 required = Pso::VertexRequiredBytes(last, slot.stride, bytes);
        constexpr u64 MaxVertexUpload = 64 * 1024 * 1024;
        if (required > MaxVertexUpload) {
            SkipDraw("vertex binding exceeds 64 MiB draw budget");
            return;
        }
        const u64 available = slot.size > attribute.offset ? slot.size - attribute.offset : 0;
        const u64 source_offset = u64{slot.offset} + attribute.offset;
        const bool realign = (source_offset % alignment) || (slot.stride % alignment) ||
                             bytes != storage_bytes || attribute.swap_rb;
        if (!realign && required <= available) {
            vertex_views[i] = {slot.resource->handle->GetGPUVirtualAddress() + source_offset,
                               static_cast<UINT>(available), slot.stride};
            need(slot.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            continue;
        }
        if (!realign) {
            // Preserve hardware zero reads beyond the inclusive Maxwell limit.
            auto zero = context.CreateBuffer(required, D3D12_HEAP_TYPE_UPLOAD);
            std::fill(zero->mapped.begin(), zero->mapped.end(), u8{0});
            auto padded = context.CreateBuffer(required, D3D12_HEAP_TYPE_DEFAULT);
            context.MarkWork("vertex binding padding");
            context.Transition(padded, D3D12_RESOURCE_STATE_COPY_DEST);
            context.Commands()->CopyBufferRegion(padded->handle.Get(), 0, zero->handle.Get(), 0,
                                                 required);
            if (available) {
                context.Transition(slot.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
                context.Commands()->CopyBufferRegion(padded->handle.Get(), 0,
                                                     slot.resource->handle.Get(), source_offset,
                                                     std::min(available, required));
            }
            context.Retain(zero);
            context.Retain(padded);
            vertex_views[i] = {padded->handle->GetGPUVirtualAddress(), static_cast<UINT>(required),
                               slot.stride};
            need(padded, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            continue;
        }
        // Repack only misaligned streams. Each attribute owns one IA slot with
        // offset zero, so shared streams cannot impose conflicting alignments.
        const u64 elements = slot.stride ? last + 1 : 1;
        if (elements * storage_bytes > MaxVertexUpload) {
            SkipDraw("realigned vertex binding exceeds 64 MiB draw budget");
            return;
        }
        auto packed = context.CreateBuffer(elements * storage_bytes, D3D12_HEAP_TYPE_UPLOAD);
        std::fill(packed->mapped.begin(), packed->mapped.end(), u8{0});
        const u64 read_bytes = std::min(available, required);
        std::vector<u8> source(static_cast<size_t>(read_bytes));
        const auto guest_address = regs.vertex_streams[attribute.slot].Address() + attribute.offset;
        if (const auto cpu = gpu_memory->GpuToCpuAddress(guest_address))
            buffer_cache.DownloadMemory(*cpu, read_bytes);
        gpu_memory->ReadBlock(guest_address, source.data(), source.size());
        Pso::RepackVertexFormat(packed->mapped.first(static_cast<size_t>(elements * storage_bytes)),
                                source, slot.stride, vertex_size,
                                static_cast<u32>(input.type.Value()), attribute.swap_rb != 0);
        context.Retain(packed);
        vertex_views[i] = {packed->handle->GetGPUVirtualAddress(),
                           static_cast<UINT>(elements * storage_bytes),
                           slot.stride ? storage_bytes : 0};
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
    const auto* feedback_info = native_pipeline->TransformFeedbackInfo();
    if (feedback_info) {
        transform_feedback.Configure(*feedback_info);
        for (u32 slot = 0; slot < regs.transform_feedback.buffers.size(); ++slot) {
            const auto& guest = regs.transform_feedback.buffers[slot];
            if (!guest.enable || !regs.transform_feedback.controls[slot].varying_count) {
                transform_feedback.BindBuffer(slot, nullptr, 0, 0);
                continue;
            }
            if (guest.size <= 0 || guest.start_offset < 0) {
                SkipDraw("invalid guest stream-output range");
                return;
            }
            const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                guest.Address() + static_cast<u32>(guest.start_offset),
                static_cast<u32>(guest.size), VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                VideoCommon::ObtainBufferOperation::MarkAsWritten);
            auto resource = buffer->Handle();
            if (!resource) {
                SkipDraw("unmapped guest stream-output buffer");
                return;
            }
            transform_feedback.BindBuffer(slot, resource, offset, static_cast<u32>(guest.size));
            need(resource, D3D12_RESOURCE_STATE_STREAM_OUT);
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

    if (!injected_draw_skip && BisectEnabled("NXBOX_D3D12_INJECT_DRAW_SKIP")) {
        injected_draw_skip = true;
        SkipDraw("injected descriptor recording failure");
        return;
    }

    // ---- 3. Fill the tables and collect resource states ----
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
        const VideoCommon::ImageViewInOut* texture_views =
            view_it + texel_count + image_buffer_count;
        for (const auto& desc : info->texture_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++srv_i, ++smp_i) {
                ImageView& view = texture_cache.GetImageView((texture_views++)->id);
                auto sampled = view.ImageHandle();
                bool color_feedback = false;
                for (const auto& color : color_resources)
                    color_feedback |= sampled && color == sampled;
                const bool depth_feedback = has_depth && sampled == depth_view->ImageHandle();
                if (color_feedback || depth_feedback) {
                    const bool stencil_writes =
                        regs.stencil_enable != 0 &&
                        ((regs.stencil_front_mask & 0xff) != 0 ||
                         (regs.stencil_two_side_enable && (regs.stencil_back_mask & 0xff) != 0));
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
                if (descriptors.WriteImageSrv(srv_table, srv_i, view,
                                              SrvDimension(desc.type, desc.is_multisample),
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

    // Render targets.
    const SIZE_T rtv_stride =
        context.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (u32 slot = 0; slot < num_rts; ++slot) {
        const auto replacement =
            color_views[slot] && color_resources[slot] != color_views[slot]->ImageHandle()
                ? color_resources[slot]
                : ResourcePtr{};
        descriptors.CreateRtv({rtv_base.ptr + slot * rtv_stride}, color_views[slot], replacement,
                              render_area.layers);
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
        descriptors.CreateDsv(dsv_handle, *depth_view, flags, render_area.layers);
    }
    // Remaining attachment/UAV and storage/geometry aliases cannot have legal
    // combined states. Reject only this draw, leaving the GPU thread alive.
    for (u32 slot = 0; slot < num_rts; ++slot) {
        if (color_views[slot])
            need(color_resources[slot], D3D12_RESOURCE_STATE_RENDER_TARGET);
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
        const auto result =
            regs.viewport_scale_offset_enabled ? MakeViewport(regs, i) : MakeFullViewport(regs);
        viewports[i] = result.viewport;
        scissors[i] = MakeScissor(regs, i);
        scissors[i].right = std::min(scissors[i].right, static_cast<LONG>(render_area.width));
        scissors[i].bottom = std::min(scissors[i].bottom, static_cast<LONG>(render_area.height));
        scissors[i].left = std::min(scissors[i].left, scissors[i].right);
        scissors[i].top = std::min(scissors[i].top, scissors[i].bottom);
        if (i == 0)
            y_flip = result.y_flip;
    }
    if (has_depth && BisectEnabled("NXBOX_D3D12_DEPTH_TRACE") && depth_trace_draws++ < 4) {
        const auto& viewport = viewports[0];
        Diagnostic(fmt::format(
            "D3D12_DEPTH_DRAW frame={} area={}x{} dsv={} depth_test={} "
            "depth_write={} compare={} stencil={} clip={} depth_mode={} "
            "z_scale={} z_translate={} viewport_z=[{},{}] indexed={} "
            "vertices={} instances={} base_vertex={} base_instance={}",
            feedback_frame, render_area.width, render_area.height, formats.depth,
            attachment_state.depth_enable, attachment_state.depth_write,
            attachment_state.depth_func, attachment_state.stencil_enable,
            attachment_state.depth_clip, static_cast<u32>(regs.depth_mode),
            regs.viewport_transform[0].scale_z, regs.viewport_transform[0].translate_z,
            viewport.MinDepth, viewport.MaxDepth, submitted.is_indexed, submitted.num_vertices,
            submitted.num_instances, submitted.base_vertex, submitted.base_instance));
    }
    commands->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    commands->RSSetScissorRects(static_cast<UINT>(scissors.size()), scissors.data());
    commands->IASetVertexBuffers(0, num_vertex_views, vertex_views.data());
    if (needs_index)
        commands->IASetIndexBuffer(&index_view);

    auto words = pipeline->RootConstants({render_area.width, render_area.height, y_flip});
    words[8] = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first;
    words[9] = draw_state.base_instance;
    words[10] = indirect_draw_id;
    const bool back_only = regs.stencil_two_side_enable && regs.gl_cull_test_enabled &&
                           static_cast<u32>(regs.gl_cull_face) == 0x404;
    const auto cull =
        Pso::CullMode(regs.gl_cull_test_enabled != 0, static_cast<u32>(regs.gl_cull_face));
    if (attachment_state.stencil_enable && Pso::DsvHasStencil(formats.depth) && regs.zeta_enable &&
        regs.stencil_enable &&
        Pso::IndependentStencil(regs.stencil_two_side_enable != 0, cull.mode,
                                regs.stencil_front_ref, regs.stencil_back_ref,
                                regs.stencil_front_func_mask, regs.stencil_back_func_mask,
                                regs.stencil_front_mask, regs.stencil_back_mask)) {
        ComPtr<ID3D12GraphicsCommandList8> commands8;
        Check(commands->QueryInterface(IID_PPV_ARGS(&commands8)),
              "D3D12 independent stencil command interface", context.Device());
        commands8->OMSetFrontAndBackStencilRef(regs.stencil_front_ref, regs.stencil_back_ref);
    } else {
        commands->OMSetStencilRef(back_only ? regs.stencil_back_ref : regs.stencil_front_ref);
    }
    const bool replicate_alpha = Pso::ConstantBlendFactors(attachment_state) == 2;
    const float blend[]{replicate_alpha ? regs.blend_color.a : regs.blend_color.r,
                        replicate_alpha ? regs.blend_color.a : regs.blend_color.g,
                        replicate_alpha ? regs.blend_color.a : regs.blend_color.b,
                        regs.blend_color.a};
    commands->OMSetBlendFactor(blend);
    if (attachment_state.depth_bounds) {
        ComPtr<ID3D12GraphicsCommandList1> commands1;
        Check(commands->QueryInterface(IID_PPV_ARGS(&commands1)),
              "D3D12 depth-bounds command interface", context.Device());
        commands1->OMSetDepthBounds(std::clamp(regs.depth_bounds[0], 0.0f, 1.0f),
                                    std::clamp(regs.depth_bounds[1], 0.0f, 1.0f));
    }
    commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(words.size()), words.data(), 0);

    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              regs.zpass_pixel_count_enable != 0);
    if (feedback_info)
        transform_feedback.Bind();
    else
        transform_feedback.Unbind(regs.transform_feedback_enabled == 0);
    query_runtime.BeginDraw(pipeline->StageInfo(3) != nullptr);
    context.MarkWork("guest draws");
    draw(commands);
    query_runtime.EndDraw();
    query_runtime.PauseHostConditionalRendering();
    for (const auto& copy : color_copies) {
        auto region = copy.region;
        std::swap(region.src_subresource, region.dst_subresource);
        texture_runtime.BlitHelpers().CopyImage(copy.original, copy.temporary,
                                                std::span{&region, 1});
    }
    if (totals.uav)
        FragmentBarrier();
    for (auto& stage : bindings.stages)
        stage.Clear(); // drop upload-page references so the staging ring can recycle them
}

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) try {
    if (BisectEnabled("NXBOX_D3D12_NO_DRAWS")) {
        m_gpu.TickWork();
        return;
    }
    converted_indices.reset();
    query_runtime.PauseHostConditionalRendering();
    const auto& state = maxwell3d->draw_manager.draw_state;
    const auto topology = static_cast<IndexConversion::Topology>(state.topology);
    const auto& regs = maxwell3d->regs;
    const bool vertex_only = !regs.IsShaderConfigEnabled(Maxwell::ShaderType::Geometry) &&
                             !regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) &&
                             !regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation);
    const auto provoking = vertex_only ? (regs.provoking_vertex == Maxwell::ProvokingVertex::Last
                                              ? IndexConversion::Provoking::Last
                                              : IndexConversion::Provoking::First)
                                       : IndexConversion::Provoking::Preserve;
    if (is_indexed &&
        static_cast<u32>(topology) <= static_cast<u32>(IndexConversion::TriangleStripAdjacency)) {
        gpu_memory->FlushCaching();
        const auto format = state.inline_index_draw_indexes.empty()
                                ? static_cast<IndexConversion::Format>(state.index_buffer.format)
                                : IndexConversion::Format::Uint32;
        if (static_cast<u32>(format) > 2) {
            SkipDraw("invalid index_buffer.format");
            return;
        }
        const size_t width = size_t{1} << static_cast<u32>(format);
        if (u64{state.index_buffer.count} * width > 64 * 1024 * 1024) {
            SkipDraw("index conversion exceeds 64 MiB draw budget");
            return;
        }
        std::vector<std::byte> input(size_t{state.index_buffer.count} * width);
        const auto address =
            state.index_buffer.StartAddress() + size_t{state.index_buffer.first} * width;
        if (!state.inline_index_draw_indexes.empty()) {
            if (input.size() > state.inline_index_draw_indexes.size()) {
                SkipDraw("inline_index_draw_indexes truncated");
                return;
            }
            std::memcpy(input.data(), state.inline_index_draw_indexes.data(), input.size());
        } else {
            if (const auto cpu = gpu_memory->GpuToCpuAddress(address)) {
                std::scoped_lock lock{buffer_cache.mutex};
                buffer_cache.DownloadMemory(*cpu, input.size());
            }
            gpu_memory->ReadBlock(address, input.data(), input.size());
        }
        converted_indices = index_converter.Convert(topology, format, input,
                                                    {maxwell3d->regs.primitive_restart.enabled != 0,
                                                     maxwell3d->regs.primitive_restart.index},
                                                    provoking);
    } else if (topology == IndexConversion::Topology::Quads ||
               topology == IndexConversion::Topology::QuadStrip ||
               topology == IndexConversion::Topology::TriangleFan ||
               topology == IndexConversion::Topology::Polygon ||
               topology == IndexConversion::Topology::LineLoop ||
               (vertex_only && static_cast<u32>(topology) <= 9)) {
        converted_indices =
            index_converter.Generate(topology, state.vertex_buffer.count, 0, provoking);
    }
    if (converted_indices && !converted_indices->count) {
        converted_indices.reset();
        m_gpu.TickWork();
        return;
    }
    DrawParams params = MakeDrawParams(state, instance_count, is_indexed);
    if (converted_indices) {
        params.num_vertices = converted_indices->count;
        params.first_index = 0;
        params.base_vertex = is_indexed ? state.base_index : state.vertex_buffer.first;
        params.is_indexed = true;
    }
    PrepareDraw(is_indexed, DrawCommand{params});
    converted_indices.reset();
    m_gpu.TickWork();
} catch (const std::exception& error) {
    converted_indices.reset();
    SkipDraw(error.what());
    m_gpu.TickWork();
} catch (...) {
    converted_indices.reset();
    SkipDraw("unknown exception in direct draw");
    m_gpu.TickWork();
}

ID3D12CommandSignature* RasterizerD3D12::IndirectSignature(bool indexed) {
    auto& signature = indexed ? indexed_signature : draw_signature;
    if (!signature) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type =
            indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride =
            indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &argument;
        Check(context.Device()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)),
              "CreateCommandSignature", context.Device());
    }
    return signature.Get();
}

void RasterizerD3D12::DrawIndirect() try {
    if (BisectEnabled("NXBOX_D3D12_NO_DRAWS")) {
        m_gpu.TickWork();
        return;
    }
    query_runtime.PauseHostConditionalRendering();
    const auto params = maxwell3d->draw_manager.indirect_state;
    if (params.is_byte_count) {
        SkipDraw("draw_auto_stride byte-count draw requires guest transform feedback linkage");
        m_gpu.TickWork();
        return;
    }
    const size_t command_size =
        params.is_indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    const size_t stride = params.stride ? params.stride : command_size;
    gpu_memory->FlushCaching();
    // Download GPU-written arguments/counts before CPU expansion. Reuse Draw so
    // every command gets its own base-ID constants, restart and topology conversion.
    const auto synchronize = [&](GPUVAddr address, size_t size) {
        if (const auto cpu = gpu_memory->GpuToCpuAddress(address)) {
            std::scoped_lock lock{buffer_cache.mutex};
            buffer_cache.DownloadMemory(*cpu, size);
        }
    };
    u32 guest_count = UINT32_MAX;
    if (params.include_count) {
        synchronize(params.count_start_address, sizeof(guest_count));
        guest_count = gpu_memory->Read<u32>(params.count_start_address);
    }
    const u64 count = Pso::IndirectCount(params.buffer_size, stride, command_size,
                                         params.max_draw_counts, guest_count);
    if (!count) {
        m_gpu.TickWork();
        return;
    }
    const size_t bytes = static_cast<size_t>((count - 1) * stride + command_size);
    if (params.indirect_start_address > UINT64_MAX - bytes) {
        SkipDraw("indirect_start_address range overflow");
        return;
    }
    synchronize(params.indirect_start_address, bytes);
    std::vector<u8> arguments(bytes);
    gpu_memory->ReadBlock(params.indirect_start_address, arguments.data(), arguments.size());
    auto& state = maxwell3d->draw_manager.draw_state;
    const auto saved_state = state;
    const auto saved_draw_id = indirect_draw_id;
    SCOPE_EXIT {
        state = saved_state;
        indirect_draw_id = saved_draw_id;
    };
    for (u64 i = 0; i < count; ++i) {
        indirect_draw_id = static_cast<u32>(i);
        const auto* data = arguments.data() + static_cast<size_t>(i) * stride;
        if (params.is_indexed) {
            D3D12_DRAW_INDEXED_ARGUMENTS command;
            std::memcpy(&command, data, sizeof(command));
            state.index_buffer.count = command.IndexCountPerInstance;
            state.index_buffer.first = command.StartIndexLocation;
            state.base_index = static_cast<u32>(command.BaseVertexLocation);
            state.base_instance = command.StartInstanceLocation;
            Draw(true, command.InstanceCount);
        } else {
            D3D12_DRAW_ARGUMENTS command;
            std::memcpy(&command, data, sizeof(command));
            state.vertex_buffer.count = command.VertexCountPerInstance;
            state.vertex_buffer.first = command.StartVertexLocation;
            state.base_instance = command.StartInstanceLocation;
            Draw(false, command.InstanceCount);
        }
    }
    m_gpu.TickWork();
} catch (const std::exception& error) {
    SkipDraw(error.what());
    m_gpu.TickWork();
} catch (...) {
    SkipDraw("unknown exception in indirect draw");
    m_gpu.TickWork();
}

bool RasterizerD3D12::AccelerateConditionalRendering() {
    gpu_memory->FlushCaching();
    return query_cache.AccelerateHostConditionalRendering();
}
void RasterizerD3D12::PreparePresentation() {
    query_runtime.PauseHostConditionalRendering();
}
void RasterizerD3D12::DrawTexture() {
    const auto execute_blit = [this] {
        ++feedback_draw;
        gpu_memory->FlushCaching();
        query_runtime.PauseHostConditionalRendering();
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.SynchronizeDescriptors(false);
        texture_cache.UpdateRenderTargets(false);
        const auto* current = texture_cache.GetFramebuffer();
        if (!current)
            return;
        Framebuffer framebuffer = *current;
        auto* target = framebuffer.Color(framebuffer.DrawBuffer(0));
        if (!target || !target->ImageHandle())
            return;
        if (!(target->ImageHandle()->handle->GetDesc().Flags &
              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
            SkipDraw("DrawTexture target lacks ALLOW_RENDER_TARGET");
            return;
        }
        query_runtime.PrepareDraw();
        const auto& blit = maxwell3d->draw_manager.draw_texture_state;
        // Descriptor discovery may relocate ImageViews; own the source snapshot.
        const ImageView source = texture_cache.GetImageView(blit.src_texture);
        const auto* sampler = texture_cache.GetSampler(blit.src_sampler, false);
        auto sampled = source.ImageHandle();
        auto description = source.Description();
        if (!sampled || !sampler)
            return;
        if (sampled->handle->GetDesc().SampleDesc.Count != 1 ||
            (description.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D &&
             description.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY) ||
            VideoCore::Surface::IsPixelFormatInteger(source.format) ||
            VideoCore::Surface::IsPixelFormatInteger(target->format)) {
            SkipDraw("draw_texture_state requires single-sample float/normalized 2D views");
            return;
        }
        texture_cache.PrepareImage(source.image_id, false, false);
        if (sampled == target->ImageHandle()) {
            sampled = CopyFeedback(sampled);
            if (!sampled) {
                SkipDraw("DrawTexture feedback copy budget");
                return;
            }
        }
        if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            const auto mip = description.Texture2D.MostDetailedMip;
            const auto levels = description.Texture2D.MipLevels;
            const auto plane = description.Texture2D.PlaneSlice;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray = {mip, levels, 0, 1, plane, 0};
        }
        Pso::GraphicsPsoState state{};
        FillGraphicsState(*maxwell3d, state);
        state.attributes.fill({});
        state.cull_mode = Pso::kCullNone;
        state.cull_all = 0;
        state.fill_mode = 3; // D3D12_FILL_MODE_SOLID
        state.topology_type = Pso::kTopologyTypeTriangle;
        state.depth_enable = state.depth_write = state.stencil_enable = state.depth_bounds = 0;
        state.logic_enable = state.alpha_to_coverage = state.conservative_raster = 0;
        state.depth_bias = 0;
        state.bias_clamp_bits = state.slope_scaled_bias_bits = 0;
        for (auto& blend : state.blend) {
            blend.enable = 0;
            blend.write_mask = 15;
        }
        Pso::FramebufferFormats formats{};
        formats.colors[0] = static_cast<u32>(target->Description().Format);
        formats.sample_count =
            static_cast<u8>(target->ImageHandle()->handle->GetDesc().SampleDesc.Count);
        Pso::ApplyFramebufferFormats(state, formats);
        const std::array<StageInput, 2> stages{{
            {Shader::Stage::VertexB, 0, Shader::Backend::HLSL::DRAW_TEXTURE_VERTEX, {}},
            {Shader::Stage::Fragment,
             0,
             Shader::Backend::HLSL::DRAW_TEXTURE_FRAGMENT,
             {0, 1, 0, 1}},
        }};
        auto& pipeline = default_pipeline_cache->GetGraphics(
            state, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, stages);
        descriptors.Reserve(3, 1, 1);
        const auto tables = descriptors.AllocateViews(3);
        const auto sampler_table = descriptors.AllocateSamplers(1);
        const auto rtv = descriptors.AllocateRtv(1);
        descriptors.WriteNullCbv(tables, 0);
        context.Device()->CreateShaderResourceView(sampled->handle.Get(), &description,
                                                   tables.Slot(1));
        descriptors.WriteNullUav(tables, 2);
        descriptors.WriteSampler(sampler_table, 0, *sampler);
        descriptors.CreateRtv(rtv, target);
        context.Transition(sampled, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context.Transition(target->ImageHandle(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        context.Retain(sampled);
        context.Retain(target->ImageHandle());
        auto* commands = context.Commands();
        descriptors.SetHeaps(commands);
        commands->SetGraphicsRootSignature(pipeline.RootSignature());
        commands->SetPipelineState(pipeline.PipelineState());
        commands->SetGraphicsRootDescriptorTable(1, tables.gpu);
        commands->SetGraphicsRootDescriptorTable(2, tables.Slice(1, 1).gpu);
        commands->SetGraphicsRootDescriptorTable(3, tables.Slice(2, 1).gpu);
        commands->SetGraphicsRootDescriptorTable(4, sampler_table.gpu);
        const auto size = framebuffer.Size();
        const std::array<float, 12> constants{blit.dst_x0,
                                              blit.dst_y0,
                                              blit.dst_x1,
                                              blit.dst_y1,
                                              blit.src_x0,
                                              blit.src_y0,
                                              blit.src_x1,
                                              blit.src_y1,
                                              static_cast<float>(size.width),
                                              static_cast<float>(size.height),
                                              static_cast<float>(source.size.width),
                                              static_cast<float>(source.size.height)};
        if (!size.width || !size.height || !source.size.width || !source.size.height)
            return;
        commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()),
                                                constants.data(), 0);
        commands->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
        const D3D12_VIEWPORT viewport{
            0, 0, static_cast<float>(size.width), static_cast<float>(size.height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(size.width),
                                 static_cast<LONG>(size.height)};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        transform_feedback.Unbind();
        query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                                  maxwell3d->regs.zpass_pixel_count_enable != 0);
        query_runtime.BeginDraw(false);
        context.MarkWork("DrawTexture blit");
        commands->DrawInstanced(4, 1, 0, 0);
        query_runtime.EndDraw();
    };
    try {
        execute_blit();
    } catch (const std::exception& error) {
        SkipDraw(error.what());
    } catch (...) {
        SkipDraw("unknown exception in DrawTexture");
    }
    // Tick callbacks may throw. Let the GPU-thread boundary cancel their waiters;
    // executing them in a noexcept scope-exit destructor could terminate instead.
    m_gpu.TickWork();
}
void RasterizerD3D12::Clear(u32 layer_count) try {
    transform_feedback.Unbind();
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
    if (depth && (selection.depth || selection.stencil) &&
        BisectEnabled("NXBOX_D3D12_DEPTH_TRACE") && depth_trace_clears++ < 4) {
        Diagnostic(fmt::format("D3D12_DEPTH_CLEAR frame={} depth={} stencil={} clear_depth={} "
                               "clear_stencil={} extent={}x{}",
                               feedback_frame, selection.depth, selection.stencil, values.depth,
                               values.stencil, depth->size.width, depth->size.height));
    }
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
            const auto flags = view->ImageHandle()->handle->GetDesc().Flags;
            const auto required = is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                           : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (!(flags & required)) {
                view->LogUnsupported(is_depth ? "clear attachment lacks ALLOW_DEPTH_STENCIL"
                                              : "clear attachment lacks ALLOW_RENDER_TARGET");
                SkipDraw("unsupported clear attachment flags");
                return false;
            }
            const auto& srv = view->Description();
            const bool msaa = view->ImageHandle()->handle->GetDesc().SampleDesc.Count > 1;
            const bool msaa_array = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            const u32 first = msaa_array ? srv.Texture2DMSArray.FirstArraySlice
                              : srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                  ? srv.Texture2DArray.FirstArraySlice
                                  : 0;
            const u32 layers = msaa_array ? srv.Texture2DMSArray.ArraySize
                               : srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                   ? srv.Texture2DArray.ArraySize
                                   : 1;
            if (layer >= layers)
                return false;
            const u32 mip = msaa ? 0
                            : srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
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
                if (msaa) {
                    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                    desc.Texture2DMSArray = {first + layer, 1};
                } else {
                    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray = {mip, first + layer, 1};
                }
                context.Device()->CreateDepthStencilView(attachment.resource->handle.Get(), &desc,
                                                         attachment.descriptor);
            } else {
                attachment.descriptor = descriptors.AllocateRtv(1);
                attachment.format = srv.Format;
                D3D12_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = srv.Format;
                if (msaa) {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    desc.Texture2DMSArray = {first + layer, 1};
                } else {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray = {mip, first + layer, 1, 0};
                }
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
} catch (const std::exception& error) {
    SkipDraw(error.what());
    m_gpu.TickWork();
} catch (...) {
    SkipDraw("unknown exception in clear");
    m_gpu.TickWork();
}
void RasterizerD3D12::DispatchCompute() try {
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
            descriptors.WriteImageSrv(srv, srv_index++, view,
                                      SrvDimension(desc.type, desc.is_multisample));
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
            if (!descriptors.WriteImageUav(
                    uav, uav_index++, view,
                    static_cast<DXGI_FORMAT>(MaxwellFormat(*format).view_format))) {
                SkipDraw("unsupported compute storage image view");
                return;
            }
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
} catch (const std::exception& error) {
    SkipDraw(error.what());
    m_gpu.TickWork();
} catch (...) {
    SkipDraw("unknown exception in compute dispatch");
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
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    // A report can be newer than overlapping cached buffer/image contents.
    // Publish queries last so downloads cannot overwrite the final guest value.
    if (True(which & VideoCommon::CacheType::QueryCache))
        query_cache.FlushRegion(addr, size);
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
    {
        std::scoped_lock lock{buffer_cache.mutex};
        // Defer cached CPU stores while the buffer still owns GPU-written bytes.
        if (buffer_cache.OnCPUWrite(addr, size))
            return true;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    default_pipeline_cache->InvalidateRegion(addr, size);
    return false;
}
void RasterizerD3D12::OnCacheInvalidation(DAddr addr, u64 size) {
    // Gathering cached CPU stores is not an explicit guest query overwrite.
    // Keep pending reports alive, as the OpenGL/Vulkan backends do.
    InvalidateRegion(addr, size,
                     VideoCommon::CacheType::BufferCache | VideoCommon::CacheType::TextureCache |
                         VideoCommon::CacheType::ShaderCache);
}
VideoCore::RasterizerDownloadArea RasterizerD3D12::GetFlushArea(PAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        if (const auto area = texture_cache.GetFlushArea(addr, size))
            return *area;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (const auto area = buffer_cache.GetFlushArea(addr, size))
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
    fence_manager.WaitPendingFences(true);
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
        u64 pool_bytes{};
        for (const auto& entry : feedback_pool)
            pool_bytes += entry.resource->allocation_size;
        Diagnostic("D3D12_FEEDBACK copies=" + std::to_string(feedback_copies) +
                   " readonly_depth=" + std::to_string(feedback_readonly_depth) +
                   " skipped_draws=" + std::to_string(skipped_draws) +
                   " frame=" + std::to_string(feedback_frame) +
                   " pool_entries=" + std::to_string(feedback_pool.size()) +
                   " pool_bytes=" + std::to_string(pool_bytes) +
                   " memory_bytes=" + std::to_string(context.MemoryUsage()));
    }
    feedback_copies = feedback_readonly_depth = skipped_draws = 0;
    std::erase_if(feedback_pool,
                  [this](const auto& entry) { return entry.frame != feedback_frame; });
    std::erase_if(attachment_pool,
                  [this](const auto& entry) { return entry.frame != feedback_frame; });
    depth_trace_draws = depth_trace_clears = 0;
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
    // The shared caches consume common dirty bits, even without a native state tracker.
    auto& dirty = channel.payload->maxwell_3d.dirty;
    VideoCommon::Dirty::SetupDirtyFlags(dirty.tables);
    dirty.flags.set();
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
    const auto log_source = [&](const ResourcePtr& resource, DXGI_FORMAT format, bool cached) {
        if (presentation_logged)
            return;
        presentation_logged = true;
        const auto desc = resource->handle->GetDesc();
        Diagnostic("D3D12_DISPLAY address=" + std::to_string(config.address + config.offset) +
                   " source=" + (cached ? "texture_cache" : "guest_memory") + " image=" +
                   std::to_string(reinterpret_cast<std::uintptr_t>(resource->handle.Get())) +
                   " format=" + std::to_string(static_cast<u32>(format)) +
                   " width=" + std::to_string(desc.Width) +
                   " height=" + std::to_string(desc.Height) + " scaled=" + std::to_string(scaled));
    };
    if (view && view->ImageHandle()) {
        texture_cache.PrepareImage(view->image_id, false, false);
        auto description = view->Description();
        if (view->ImageHandle()->handle->GetDesc().SampleDesc.Count > 1) {
            auto resource = texture_runtime.ResolveFramebuffer(*view);
            if (!resource)
                return {};
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray = {0, 1, 0, 1, 0, 0};
            log_source(resource, description.Format, true);
            return {resource, description};
        }
        if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            const auto mip = description.Texture2D.MostDetailedMip;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray = {mip, 1, 0, 1, 0, 0};
        } else if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
            description.Texture2DArray.ArraySize = 1;
            description.Texture2DArray.MipLevels = 1;
        } else {
            view->LogUnsupported("framebuffer must be a 2D texture view");
            return {};
        }
        log_source(view->ImageHandle(), description.Format, true);
        return {view->ImageHandle(), description};
    }
    auto resource = texture_runtime.UploadFramebuffer(device_memory, config);
    if (!resource)
        return {};
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = static_cast<DXGI_FORMAT>(
        MaxwellFormat(VideoCore::Surface::PixelFormatFromGPUPixelFormat(config.pixel_format))
            .view_format);
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2DArray = {0, 1, 0, 1, 0, 0};
    log_source(resource, description.Format, false);
    return {resource, description};
}
} // namespace D3D12
