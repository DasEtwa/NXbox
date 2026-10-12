// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <exception>
#include <bit>
#include <cmath>
#include <cstring>
#include <tuple>
#include <vector>

#include "common/logging.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/stub_hlsl_emit.h"
#include "video_core/surface.h"

namespace D3D12 {
namespace {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

u8 StencilFaceMask(u32 value) {
    return static_cast<u8>(value & 0xFF);
}

Pso::StencilFaceState Face(const Maxwell::StencilOp& op) {
    return {static_cast<u8>(Pso::StencilOp(static_cast<u32>(op.fail))),
            static_cast<u8>(Pso::StencilOp(static_cast<u32>(op.zfail))),
            static_cast<u8>(Pso::StencilOp(static_cast<u32>(op.zpass))),
            static_cast<u8>(Pso::ComparisonFunc(static_cast<u32>(op.func)))};
}

template <typename Blend>
void SetBlend(Pso::BlendTargetState& out, const Blend& b) {
    out.enable = 1;
    out.src = static_cast<u8>(Pso::BlendFactor(static_cast<u32>(b.color_source)));
    out.dst = static_cast<u8>(Pso::BlendFactor(static_cast<u32>(b.color_dest)));
    out.op = static_cast<u8>(Pso::BlendOp(static_cast<u32>(b.color_op)));
    out.src_alpha = static_cast<u8>(Pso::BlendFactor(static_cast<u32>(b.alpha_source)));
    out.dst_alpha = static_cast<u8>(Pso::BlendFactor(static_cast<u32>(b.alpha_dest)));
    out.op_alpha = static_cast<u8>(Pso::BlendOp(static_cast<u32>(b.alpha_op)));
}

} // namespace

bool GraphicsPipelineKey::operator==(const GraphicsPipelineKey& rhs) const noexcept {
    return std::memcmp(this, &rhs, sizeof(*this)) == 0;
}

void FillGraphicsState(Tegra::Engines::Maxwell3D& maxwell3d, Pso::GraphicsPsoState& s) {
    const Maxwell& regs = maxwell3d.regs;
    std::memset(&s, 0, sizeof(s));

    const u32 topology = static_cast<u32>(maxwell3d.draw_manager.draw_state.topology);
    s.topology_type = static_cast<u8>(Pso::TopologyType(topology));
    s.patch_vertices = static_cast<u8>(regs.patch_vertices);

    // Rasterizer. Front face flips with the window origin like the Vulkan backend.
    const auto cull = Pso::CullMode(regs.gl_cull_test_enabled != 0,
                                    static_cast<u32>(regs.gl_cull_face));
    s.cull_mode = static_cast<u8>(cull.mode);
    s.cull_all = cull.cull_all;
    bool ccw = regs.gl_front_face == Maxwell::FrontFace::CounterClockWise;
    if (regs.window_origin.flip_y != 0) {
        ccw = !ccw;
    }
    s.front_ccw = ccw;
    s.fill_mode = regs.polygon_mode_front == Maxwell::PolygonMode::Fill ? 3 : 2; // no point mode
    using Clip = Maxwell::ViewportClipControl::GeometryClip;
    const auto geometry_clip = regs.viewport_clip_control.geometry_clip;
    s.depth_clip = geometry_clip == Clip::Passthrough || geometry_clip == Clip::FrustumXYZ ||
                   geometry_clip == Clip::FrustumZ;
    // Polygon offset is enabled per primitive class (point / line / fill).
    const u32 class_ = Pso::TopologyType(topology);
    const bool bias = class_ == Pso::kTopologyTypePoint  ? regs.polygon_offset_point_enable != 0
                      : class_ == Pso::kTopologyTypeLine ? regs.polygon_offset_line_enable != 0
                                                         : regs.polygon_offset_fill_enable != 0;
    if (bias) {
        // GL/Maxwell constant units are "r" like D3D12's integer DepthBias.
        s.depth_bias = static_cast<s32>(std::lround(regs.depth_bias));
        s.slope_scaled_bias_bits = std::bit_cast<u32>(regs.slope_scale_depth_bias);
        s.bias_clamp_bits = std::bit_cast<u32>(regs.depth_bias_clamp);
    }

    // Depth-stencil.
    s.depth_enable = regs.zeta_enable != 0 && regs.depth_test_enable != 0;
    s.depth_write = regs.zeta_enable != 0 && regs.depth_write_enabled != 0;
    s.depth_func = static_cast<u8>(Pso::ComparisonFunc(static_cast<u32>(regs.depth_test_func)));
    s.stencil_enable = regs.zeta_enable != 0 && regs.stencil_enable != 0;
    s.stencil_read_mask = StencilFaceMask(regs.stencil_front_func_mask);
    s.stencil_write_mask = StencilFaceMask(regs.stencil_front_mask);
    s.front = Face(regs.stencil_front_op);
    s.back = regs.stencil_two_side_enable ? Face(regs.stencil_back_op) : s.front;

    // Blend (independent per render target).
    s.independent_blend = 1;
    for (size_t i = 0; i < Pso::kNumRenderTargets; ++i) {
        auto& out = s.blend[i];
        out = {0, 0xF, Pso::kBlendOne, Pso::kBlendZero, 1, Pso::kBlendOne, Pso::kBlendZero, 1};
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : i];
        out.write_mask = static_cast<u8>((mask.R ? 1 : 0) | (mask.G ? 2 : 0) | (mask.B ? 4 : 0) |
                                         (mask.A ? 8 : 0));
        if (regs.blend.enable[i]) {
            if (regs.blend_per_target_enabled) {
                SetBlend(out, regs.blend_per_target[i]);
            } else {
                SetBlend(out, regs.blend);
            }
        }
    }

    // Render target / depth formats from the texture cache's table.
    const u32 count = std::min<u32>(regs.rt_control.count, Pso::kNumRenderTargets);
    s.num_render_targets = static_cast<u8>(count);
    for (u32 i = 0; i < count; ++i) {
        const auto& rt = regs.rt[regs.rt_control.Map(i)];
        if (rt.format != Tegra::RenderTargetFormat::NONE) {
            s.rtv_formats[i] =
                MaxwellFormat(VideoCore::Surface::PixelFormatFromRenderTargetFormat(rt.format))
                    .view_format;
        }
    }
    if (regs.zeta_enable != 0) {
        s.dsv_format = Pso::DsvFormat(
            MaxwellFormat(VideoCore::Surface::PixelFormatFromDepthFormat(regs.zeta.format))
                .resource_format);
    }
    s.sample_count = 1; // TODO: MSAA (regs.anti_alias_samples_mode)

    // Vertex input.
    for (size_t i = 0; i < Pso::kNumVertexAttributes; ++i) {
        const auto& in = regs.vertex_attrib_format[i];
        auto& out = s.attributes[i];
        const auto format = Pso::VertexFormatFor(static_cast<u32>(in.size.Value()),
                                                 static_cast<u32>(in.type.Value()), in.bgra != 0);
        out.enabled = in.constant == 0 && format.dxgi != 0;
        out.slot = static_cast<u8>(in.buffer);
        out.offset = static_cast<u16>(in.offset);
        out.dxgi = format.dxgi;
        out.instanced = regs.vertex_stream_instances.IsInstancingEnabled(in.buffer);
        out.swap_rb = format.swap_rb;
        out.scaled = format.scaled;
    }
}

ID3D12RootSignature* PipelineCache::GetRootSignature(DescriptorCounts counts) {
    auto it = root_signatures.find(counts);
    if (it == root_signatures.end()) {
        it = root_signatures.emplace(counts, CreateRootSignature(context.Device(), counts)).first;
    }
    return it->second.Get();
}

GraphicsPipeline& PipelineCache::GetGraphics(const Pso::GraphicsPsoState& state, u32 d3d_topology,
                                             std::span<const StageInput> stages) {
    GraphicsPipelineKey key{};
    key.state = state;
    DescriptorCounts total;
    for (const auto& stage : stages) {
        key.stage_hashes[static_cast<size_t>(stage.stage) % key.stage_hashes.size()] = stage.hash;
        total += stage.counts;
    }
    if (const auto it = graphics.find(key); it != graphics.end()) {
        return *it->second;
    }
    std::vector<StageProgram> programs;
    for (const auto& stage : stages) {
        programs.push_back({stage.stage, dxc.Compile(stage.hlsl, stage.stage)});
    }
    ID3D12RootSignature* signature = GetRootSignature(total);
    auto pipeline = std::make_unique<GraphicsPipeline>(context.Device(), signature, state, d3d_topology, programs);
    return *graphics.emplace(key, std::move(pipeline)).first->second;
}

ComputePipeline& PipelineCache::GetCompute(const StageInput& stage) {
    if (const auto it = compute.find(stage.hash); it != compute.end()) {
        return *it->second;
    }
    auto dxil = dxc.Compile(stage.hlsl, Shader::Stage::Compute);
    auto pipeline = std::make_unique<ComputePipeline>(
        context.Device(), GetRootSignature(stage.counts), dxil);
    return *compute.emplace(stage.hash, std::move(pipeline)).first->second;
}

IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    if (!maxwell3d) {
        return nullptr;
    }
    // TODO(integration): replace the stub stages with the translated guest shaders (Maxwell ->
    // IR -> TranslateToHlsl) and call SetStageInfo with each program's Info.
    static constexpr std::array kStubStages{Shader::Stage::VertexB, Shader::Stage::Fragment};
    std::array<StageInput, kStubStages.size()> stages{};
    for (size_t i = 0; i < kStubStages.size(); ++i) {
        const auto stage = kStubStages[i];
        stages[i] = {stage, 0x5eb0'0000u + static_cast<u64>(stage), StubHlsl(stage), {}};
    }
    Pso::GraphicsPsoState state;
    FillGraphicsState(*maxwell3d, state);
    const auto topology = Pso::PrimitiveTopology(
        static_cast<u32>(maxwell3d->draw_manager.draw_state.topology), state.patch_vertices);
    try {
        return &GetGraphics(state, topology.topology, stages);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 pipeline creation failed: {}", error.what());
        return nullptr;
    }
}

} // namespace D3D12
