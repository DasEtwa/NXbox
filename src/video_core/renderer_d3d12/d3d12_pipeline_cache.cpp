// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <set>
#include <tuple>
#include <vector>
#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/program_header.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/surface.h"

namespace D3D12 {
namespace {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

void ReportUnsupported(std::string_view reason) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::scoped_lock lock{mutex};
    if (reported.emplace(reason).second) {
        Diagnostic(fmt::format("D3D12_STATE_FALLBACK reason={} action=skip_draw", reason));
        LOG_WARNING(Render, "D3D12 state fallback: {}", reason);
    }
}

// A rejected state must not produce a valid-looking PSO with different semantics.
// These cases need shader linkage/emulation beyond the fixed-function D3D12 API.
const char* UnsupportedState(const Maxwell& regs, u32 topology) {
    if (topology > 14)
        return "invalid Maxwell PrimitiveTopology";
    if (!regs.rasterize_enable)
        return "rasterize_enable=0 requires discard with vertex/query side effects";
    if (regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
        regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation))
        return "tessellation.params require HLSL hull/domain stage linkage";
    if (regs.transform_feedback_enabled)
        return "transform_feedback/stream_out_layout require HLSL linkage and stream-output root "
               "signature";
    if (regs.depth_bounds_enable &&
        (!std::isfinite(regs.depth_bounds[0]) || !std::isfinite(regs.depth_bounds[1]) ||
         regs.depth_bounds[0] > regs.depth_bounds[1]))
        return "depth_bounds_enable/depth_bounds invalid range";
    if (regs.anti_alias_alpha_control.alpha_to_one)
        return "anti_alias_alpha_control.alpha_to_one requires fragment epilogue";
    if (regs.iterated_blend.enable || regs.iterated_blend.enable_alpha)
        return "iterated_blend requires programmable blend emulation";
    if (regs.polygon_mode_front == Maxwell::PolygonMode::Point)
        return "polygon_mode_front=Point requires point rasterization emulation";
    if (regs.polygon_mode_front != regs.polygon_mode_back)
        return "polygon_mode_back differs from polygon_mode_front; requires per-face polygon "
               "emulation";
    if (topology == 0 && regs.point_size != 1.0f)
        return "point_size requires expanded point geometry";
    if (Pso::TopologyType(topology) == Pso::kTopologyTypeLine) {
        if (regs.line_stipple_enable)
            return "line_stipple_enable/line_stipple_params require fragment stipple emulation";
        const float width =
            regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased;
        if (width != 1.0f)
            return "line_width_smooth/line_width_aliased require expanded line geometry";
    }
    if (regs.viewport_scale_offset_enabled) {
        for (const auto& viewport : regs.viewport_transform) {
            if (viewport.swizzle.x != Maxwell::ViewportSwizzle::PositiveX ||
                (viewport.swizzle.y != Maxwell::ViewportSwizzle::PositiveY &&
                 viewport.swizzle.y != Maxwell::ViewportSwizzle::NegativeY) ||
                viewport.swizzle.z != Maxwell::ViewportSwizzle::PositiveZ ||
                viewport.swizzle.w != Maxwell::ViewportSwizzle::PositiveW || viewport.scale_x < 0)
                return "viewport_transform.swizzle requires general position swizzle emulation";
        }
    }
    if (!std::isfinite(regs.depth_bias) || !std::isfinite(regs.slope_scale_depth_bias) ||
        !std::isfinite(regs.depth_bias_clamp) ||
        static_cast<double>(regs.depth_bias) / 2.0 < std::numeric_limits<s32>::min() ||
        static_cast<double>(regs.depth_bias) / 2.0 > std::numeric_limits<s32>::max())
        return "depth_bias/slope_scale_depth_bias/depth_bias_clamp invalid or outside D3D12 range";
    return nullptr;
}

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
    out.src_alpha = static_cast<u8>(Pso::AlphaBlendFactor(static_cast<u32>(b.alpha_source)));
    out.dst_alpha = static_cast<u8>(Pso::AlphaBlendFactor(static_cast<u32>(b.alpha_dest)));
    out.op_alpha = static_cast<u8>(Pso::BlendOp(static_cast<u32>(b.alpha_op)));
}

void Append32(DiskBytes& bytes, u32 value) {
    for (u32 i = 0; i < 4; ++i)
        bytes.push_back(static_cast<u8>(value >> (i * 8)));
}
void Append64(DiskBytes& bytes, u64 value) {
    Append32(bytes, static_cast<u32>(value));
    Append32(bytes, static_cast<u32>(value >> 32));
}
DiskBytes Recipe(const Pso::GraphicsPsoState* state, u32 topology,
                 std::span<const StageInput> stages) {
    DiskBytes bytes;
    Append32(bytes, 2);
    Append32(bytes, topology);
    if (state) {
        const auto* data = reinterpret_cast<const u8*>(state);
        bytes.insert(bytes.end(), data, data + sizeof(*state));
    }
    Append32(bytes, static_cast<u32>(stages.size()));
    for (const auto& stage : stages) {
        Append32(bytes, static_cast<u32>(stage.stage));
        Append64(bytes, stage.hash);
        Append32(bytes, stage.counts.cbv);
        Append32(bytes, stage.counts.srv);
        Append32(bytes, stage.counts.uav);
        Append32(bytes, stage.counts.sampler);
        Append32(bytes, static_cast<u32>(stage.hlsl.size()));
        bytes.insert(bytes.end(), stage.hlsl.begin(), stage.hlsl.end());
    }
    return bytes;
}
class RecipeReader {
public:
    explicit RecipeReader(std::span<const u8> data_) : data{data_} {}
    std::span<const u8> Read(size_t size) {
        if (size > data.size())
            throw std::runtime_error("D3D12 truncated PSO recipe");
        const auto result = data.first(size);
        data = data.subspan(size);
        return result;
    }
    u32 U32() {
        const auto bytes = Read(4);
        return u32{bytes[0]} | (u32{bytes[1]} << 8) | (u32{bytes[2]} << 16) | (u32{bytes[3]} << 24);
    }
    u64 U64() {
        const u64 low = U32();
        return low | (u64{U32()} << 32);
    }
    bool Empty() const {
        return data.empty();
    }

private:
    std::span<const u8> data;
};
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
    const auto cull =
        Pso::CullMode(regs.gl_cull_test_enabled != 0, static_cast<u32>(regs.gl_cull_face));
    s.cull_mode = static_cast<u8>(cull.mode);
    s.cull_all = cull.cull_all && (s.topology_type == Pso::kTopologyTypeTriangle ||
                                   s.topology_type == Pso::kTopologyTypePatch);
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
        s.depth_bias = static_cast<s32>(std::lround(regs.depth_bias / 2.0f));
        s.slope_scaled_bias_bits = std::bit_cast<u32>(regs.slope_scale_depth_bias);
        s.bias_clamp_bits = std::bit_cast<u32>(regs.depth_bias_clamp);
    }

    // Depth-stencil.
    s.depth_enable = regs.zeta_enable != 0 && regs.depth_test_enable != 0;
    s.depth_write = regs.zeta_enable != 0 && regs.depth_write_enabled != 0;
    s.depth_func = static_cast<u8>(Pso::ComparisonFunc(static_cast<u32>(regs.depth_test_func)));
    s.stencil_enable = regs.zeta_enable != 0 && regs.stencil_enable != 0;
    const bool back_only = regs.stencil_two_side_enable && cull.mode == Pso::kCullFront;
    s.stencil_read_mask =
        StencilFaceMask(back_only ? regs.stencil_back_func_mask : regs.stencil_front_func_mask);
    s.stencil_write_mask =
        StencilFaceMask(back_only ? regs.stencil_back_mask : regs.stencil_front_mask);
    s.front = Face(regs.stencil_front_op);
    s.back = regs.stencil_two_side_enable ? Face(regs.stencil_back_op) : s.front;
    s.stencil_back_read_mask = regs.stencil_two_side_enable
                                   ? StencilFaceMask(regs.stencil_back_func_mask)
                                   : s.stencil_read_mask;
    s.stencil_back_write_mask = regs.stencil_two_side_enable
                                    ? StencilFaceMask(regs.stencil_back_mask)
                                    : s.stencil_write_mask;
    if (s.stencil_enable &&
        Pso::IndependentStencil(regs.stencil_two_side_enable != 0, cull.mode,
                                regs.stencil_front_ref, regs.stencil_back_ref,
                                regs.stencil_front_func_mask, regs.stencil_back_func_mask,
                                regs.stencil_front_mask, regs.stencil_back_mask))
        s.stencil_enable |= 2; // Bit 1 selects native independent face state.

    // Blend (independent per render target).
    s.logic_enable = regs.logic_op.enable != 0;
    s.logic_op = static_cast<u8>(Pso::LogicOp(static_cast<u32>(regs.logic_op.op)));
    s.independent_blend = !s.logic_enable;
    s.alpha_to_coverage = regs.anti_alias_alpha_control.alpha_to_coverage != 0;
    s.conservative_raster = regs.conservative_raster_enable != 0;
    s.smooth_lines = regs.line_anti_alias_enable != 0;
    s.depth_bounds = regs.zeta_enable && regs.depth_bounds_enable;
    for (size_t i = 0; i < Pso::kNumRenderTargets; ++i) {
        auto& out = s.blend[i];
        out = {0, 0xF, Pso::kBlendOne, Pso::kBlendZero, 1, Pso::kBlendOne, Pso::kBlendZero, 1};
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : i];
        out.write_mask = static_cast<u8>((mask.R ? 1 : 0) | (mask.G ? 2 : 0) | (mask.B ? 4 : 0) |
                                         (mask.A ? 8 : 0));
        if (!s.logic_enable && regs.blend.enable[i]) {
            if (regs.blend_per_target_enabled) {
                SetBlend(out, regs.blend_per_target[i]);
            } else {
                SetBlend(out, regs.blend);
            }
        }
    }

    s.sample_count = 1; // Overridden by the actual bound attachment sample count.

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
        out.divisor = regs.vertex_streams[in.buffer].frequency;
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
    std::scoped_lock lock{pipeline_mutex};
    GraphicsPipelineKey key{};
    key.stage_hashes[5] = d3d_topology;
    key.state = state;
    DescriptorCounts total;
    for (const auto& stage : stages) {
        key.stage_hashes[static_cast<size_t>(stage.stage)] =
            Pso::HashBytes(stage.hlsl.data(), stage.hlsl.size(), stage.hash);
        total += stage.counts;
    }
    if (const auto it = graphics.find(key); it != graphics.end()) {
        return *it->second;
    }
    std::vector<StageProgram> programs;
    for (const auto& stage : stages) {
        programs.push_back(
            {stage.stage, dxc.Compile(stage.hlsl, stage.stage, Pso::HashBytes(&key, sizeof(key)))});
    }
    ID3D12RootSignature* signature = GetRootSignature(total);
    auto pipeline = std::make_unique<GraphicsPipeline>(
        context.Device(), signature, state, d3d_topology, programs, disk.get(),
        Pso::HashBytes(&key, sizeof(key)), Recipe(&state, d3d_topology, stages));
    return *graphics.emplace(key, std::move(pipeline)).first->second;
}

ComputePipeline& PipelineCache::GetCompute(const StageInput& stage) {
    std::scoped_lock lock{pipeline_mutex};
    const u64 key = Pso::HashBytes(stage.hlsl.data(), stage.hlsl.size(), stage.hash);
    if (const auto it = compute.find(key); it != compute.end()) {
        return *it->second;
    }
    auto dxil = dxc.Compile(stage.hlsl, Shader::Stage::Compute, key);
    auto pipeline = std::make_unique<ComputePipeline>(
        context.Device(), GetRootSignature(stage.counts), dxil, disk.get(), key,
        Recipe(nullptr, 0, std::span{&stage, size_t{1}}));
    return *compute.emplace(key, std::move(pipeline)).first->second;
}

namespace {
struct TranslationState {
    Shader::ObjectPool<Shader::IR::Inst> inst;
    Shader::ObjectPool<Shader::IR::Block> block;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
    Shader::Profile profile;
    Shader::HostTranslateInfo host;

    explicit TranslationState(ID3D12Device* device) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
              "D3D12 shader options");
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 waves{};
        Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &waves, sizeof(waves)),
              "D3D12 wave options");
        if (!waves.WaveOps || waves.WaveLaneCountMin < 32) {
            throw Shader::NotImplementedException("D3D12 requires waves of at least 32 lanes");
        }
        host.support_float64 = options.DoublePrecisionFloatShaderOps != FALSE;
        host.support_int64 = waves.Int64ShaderOps != FALSE;
        host.support_viewport_index_layer =
            options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation !=
            FALSE;
        host.min_ssbo_alignment = 16;
        host.ApplyDescriptorLimitPolicy();
        profile.support_int64 = host.support_int64;
        profile.support_vertex_instance_id = true;
        profile.support_vote = true;
        profile.support_quad_shuffles = true;
        profile.support_derivative_control = true;
        profile.support_viewport_index_layer_non_geometry = host.support_viewport_index_layer;
        profile.warp_size_potentially_larger_than_guest = waves.WaveLaneCountMax > 32;
        profile.max_user_clip_distances = 8;
        profile.min_ssbo_alignment = 16;
    }
};

Shader::RuntimeInfo GraphicsRuntime(const Maxwell& regs, const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous) {
    Shader::RuntimeInfo runtime;
    runtime.force_early_z = regs.mandated_early_z != 0;
    if (program.stage == Shader::Stage::Fragment && regs.alpha_test_enabled &&
        Pso::ComparisonFunc(static_cast<u32>(regs.alpha_test_func)) != 8) {
        runtime.alpha_test_func = static_cast<Shader::CompareFunction>(
            Pso::ComparisonFunc(static_cast<u32>(regs.alpha_test_func)) - 1);
        runtime.alpha_test_reference = regs.alpha_test_ref;
    }
    if (program.stage == Shader::Stage::Fragment && regs.blend.enable[0]) {
        const auto dual_source = [](const auto& blend) {
            const auto is_dual = [](auto factor) {
                const u32 native = Pso::BlendFactor(static_cast<u32>(factor));
                return native >= 16 && native <= 19;
            };
            return is_dual(blend.color_source) || is_dual(blend.color_dest) ||
                   is_dual(blend.alpha_source) || is_dual(blend.alpha_dest);
        };
        runtime.dual_source_blend = regs.blend_per_target_enabled
                                        ? dual_source(regs.blend_per_target[0])
                                        : dual_source(regs.blend);
    }
    if (regs.transform_feedback_enabled)
        throw Shader::NotImplementedException("D3D12 guest transform feedback linkage");
    if (previous) {
        runtime.previous_stage_stores = previous->info.stores;
        runtime.previous_stage_legacy_stores_mapping = previous->info.legacy_stores_mapping;
    } else {
        runtime.previous_stage_stores.mask.set();
    }
    const auto& viewport = regs.viewport_transform[0];
    bool positive_height = !regs.viewport_scale_offset_enabled || viewport.scale_y > 0;
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft)
        positive_height = !positive_height;
    if (regs.viewport_scale_offset_enabled &&
        viewport.swizzle.y == Maxwell::ViewportSwizzle::NegativeY)
        positive_height = !positive_height;
    runtime.y_negate = program.stage == Shader::Stage::VertexB && positive_height;
    runtime.convert_depth_mode = program.stage == Shader::Stage::VertexB &&
                                 regs.depth_mode == Maxwell::DepthMode::MinusOneToOne;
    for (size_t i = 0; i < runtime.generic_input_types.size(); ++i) {
        const auto& attribute = regs.vertex_attrib_format[i];
        if (attribute.constant) {
            runtime.generic_input_types[i] = Shader::AttributeType::Disabled;
            continue;
        }
        using Type = Maxwell::VertexAttribute::Type;
        switch (attribute.type.Value()) {
        case Type::SInt:
            runtime.generic_input_types[i] = Shader::AttributeType::SignedInt;
            break;
        case Type::UInt:
            runtime.generic_input_types[i] = Shader::AttributeType::UnsignedInt;
            break;
        case Type::SScaled:
            runtime.generic_input_types[i] = Shader::AttributeType::SignedScaled;
            break;
        case Type::UScaled:
            runtime.generic_input_types[i] = Shader::AttributeType::UnsignedScaled;
            break;
        default:
            runtime.generic_input_types[i] = Shader::AttributeType::Float;
            break;
        }
    }
    for (size_t i = 0; i < runtime.color_output_types.size(); ++i) {
        const auto format = regs.rt[regs.rt_control.Map(static_cast<u32>(i))].format;
        if (format == Tegra::RenderTargetFormat::NONE)
            continue;
        const auto pixel = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
        runtime.color_output_types[i] = VideoCore::Surface::IsPixelFormatInteger(pixel)
                                            ? (VideoCore::Surface::IsPixelFormatSignedInteger(pixel)
                                                   ? Shader::AttributeType::SignedInt
                                                   : Shader::AttributeType::UnsignedInt)
                                            : Shader::AttributeType::Float;
    }
    return runtime;
}
} // namespace

IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(
    const Pso::FramebufferFormats& formats) try {
    if (!maxwell3d)
        return nullptr;
    std::scoped_lock lock{pipeline_mutex};
    if (const auto* reason = UnsupportedState(
            maxwell3d->regs, static_cast<u32>(maxwell3d->draw_manager.draw_state.topology))) {
        ReportUnsupported(reason);
        return nullptr;
    }
    SyncGuestHost();
    if (!RefreshStages(guest_hashes))
        return nullptr;
    Pso::GraphicsPsoState state;
    FillGraphicsState(*maxwell3d, state);
    Pso::ApplyFramebufferFormats(state, formats);
    if (state.stencil_enable & 2) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS14 options{};
        if (FAILED(context.Device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options,
                                                         sizeof(options))) ||
            !options.IndependentFrontAndBackStencilRefMaskSupported) {
            ReportUnsupported("stencil_back_ref/stencil_back_mask/stencil_back_func_mask differ; "
                              "host lacks IndependentFrontAndBackStencilRefMaskSupported");
            return nullptr;
        }
    }
    if (state.depth_bounds) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
        if (FAILED(context.Device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options,
                                                         sizeof(options))) ||
            !options.DepthBoundsTestSupported) {
            ReportUnsupported("depth_bounds_enable/depth_bounds unsupported by host device");
            return nullptr;
        }
    }
    const u32 constant_factors = Pso::ConstantBlendFactors(state);
    if (constant_factors & 2) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS13 options{};
        if (FAILED(context.Device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options,
                                                         sizeof(options))) ||
            !options.AlphaBlendFactorSupported) {
            if (constant_factors & 1) {
                ReportUnsupported(
                    "blend mixes constant-color and constant-alpha RGB factors; host lacks "
                    "AlphaBlendFactorSupported and needs shader blend emulation");
                return nullptr;
            }
            // The draw path replicates blend_color.a into RGB. No shader change
            // is needed when RGB never consumes the original constant color.
            Pso::EmulateConstantAlpha(state);
        }
    }
    if (state.logic_enable || state.conservative_raster) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (FAILED(context.Device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                         sizeof(options))) ||
            (state.logic_enable && !options.OutputMergerLogicOp) ||
            (state.conservative_raster &&
             options.ConservativeRasterizationTier ==
                 D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED)) {
            ReportUnsupported("logic_op/conservative_raster_enable unsupported by host device");
            return nullptr;
        }
    }
    if (state.logic_enable) {
        std::optional<u8> mask;
        for (size_t i = 0; i < formats.colors.size(); ++i) {
            if (!formats.colors[i])
                continue;
            if (mask && *mask != state.blend[i].write_mask) {
                ReportUnsupported(
                    "logic_op with independent color_mask requires programmable blend emulation");
                return nullptr;
            }
            mask = state.blend[i].write_mask;
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{static_cast<DXGI_FORMAT>(formats.colors[i])};
            if (FAILED(context.Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                             sizeof(support))) ||
                !(support.Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP)) {
                ReportUnsupported("logic_op RTV format lacks OUTPUT_MERGER_LOGIC_OP; requires "
                                  "programmable blend emulation");
                return nullptr;
            }
        }
        if (mask)
            state.blend[0].write_mask = *mask;
    }
    const auto topology = Pso::PrimitiveTopology(
        static_cast<u32>(maxwell3d->draw_manager.draw_state.topology), state.patch_vertices);
    GraphicsPipelineKey request{guest_hashes, state};
    const auto& regs = maxwell3d->regs;
    u64 specialization = Pso::HashBytes(&regs.depth_mode, sizeof(regs.depth_mode));
    specialization =
        Pso::HashBytes(&regs.window_origin, sizeof(regs.window_origin), specialization);
    specialization = Pso::HashBytes(&regs.viewport_scale_offset_enabled,
                                    sizeof(regs.viewport_scale_offset_enabled), specialization);
    const auto& viewport = regs.viewport_transform[0];
    const bool positive_y = viewport.scale_y > 0;
    specialization = Pso::HashBytes(&positive_y, sizeof(positive_y), specialization);
    specialization = Pso::HashBytes(&viewport.swizzle, sizeof(viewport.swizzle), specialization);
    specialization = Pso::HashBytes(regs.vertex_attrib_format.data(),
                                    sizeof(regs.vertex_attrib_format), specialization);
    specialization = Pso::HashBytes(&topology.topology, sizeof(topology.topology), specialization);
    specialization =
        Pso::HashBytes(&regs.provoking_vertex, sizeof(regs.provoking_vertex), specialization);
    specialization =
        Pso::HashBytes(&regs.mandated_early_z, sizeof(regs.mandated_early_z), specialization);
    specialization =
        Pso::HashBytes(&regs.alpha_test_enabled, sizeof(regs.alpha_test_enabled), specialization);
    specialization =
        Pso::HashBytes(&regs.alpha_test_func, sizeof(regs.alpha_test_func), specialization);
    specialization =
        Pso::HashBytes(&regs.alpha_test_ref, sizeof(regs.alpha_test_ref), specialization);
    specialization = Pso::HashBytes(&regs.transform_feedback_enabled,
                                    sizeof(regs.transform_feedback_enabled), specialization);
    request.stage_hashes[0] =
        Pso::HashBytes(&specialization, sizeof(specialization), request.stage_hashes[0]);
    if (const auto found = guest_graphics.find(request); found != guest_graphics.end())
        return found->second;
    try {
        GraphicsEnvironments environments;
        GetGraphicsEnvironments(environments, guest_hashes);
        TranslationState translation{context.Device()};
        std::array<Shader::IR::Program, 6> programs;
        std::array<std::string, 6> sources;
        std::vector<StageInput> stages;
        for (size_t index = 0; index < programs.size(); ++index) {
            if (!guest_hashes[index])
                continue;
            auto& env = environments.envs[index];
            Shader::Maxwell::Flow::CFG cfg{
                env, translation.flow,
                static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader)), index == 0};
            programs[index] = Shader::Maxwell::TranslateProgram(translation.inst, translation.block,
                                                                env, cfg, translation.host);
            if (index == 1 && guest_hashes[0]) {
                programs[index] =
                    Shader::Maxwell::MergeDualVertexPrograms(programs[0], programs[1], env);
            }
        }
        Shader::Backend::Bindings bindings;
        const Shader::IR::Program* previous{};
        for (size_t index = guest_hashes[0] && guest_hashes[1] ? 1 : 0; index < 6; ++index) {
            if (!guest_hashes[index])
                continue;
            auto& program = programs[index];
            if (index == 0)
                program.stage = Shader::Stage::VertexB;
            if (program.stage == Shader::Stage::Fragment) {
                // Guest shaders may export more colors than this framebuffer binds.
                // Specialize the DXIL signature together with the PSO attachment key.
                for (size_t slot = 0; slot < formats.colors.size(); ++slot)
                    program.info.stores_frag_color[slot] &= formats.colors[slot] != 0;
            }
            const auto runtime = GraphicsRuntime(maxwell3d->regs, program, previous);
            Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
            if (program.stage == Shader::Stage::Fragment &&
                maxwell3d->draw_manager.draw_state.topology == Maxwell::PrimitiveTopology::Quads &&
                regs.provoking_vertex == Maxwell::ProvokingVertex::Last) {
                for (size_t attribute = 0; attribute < program.info.interpolation.size();
                     ++attribute) {
                    if (program.info.loads.Generic(attribute) &&
                        program.info.interpolation[attribute] == Shader::Interpolation::Flat) {
                        ReportUnsupported("provoking_vertex=Last for flat quads requires "
                                          "flat-value replication without changing diagonal");
                        guest_graphics.emplace(request, nullptr);
                        return nullptr;
                    }
                }
            }
            sources[index] =
                Shader::Backend::HLSL::EmitHLSL(translation.profile, runtime, program, bindings);
            const auto counts = CountStageTables(program.info);
            stages.push_back({program.stage,
                              guest_hashes[index],
                              sources[index],
                              {counts.cbv, counts.srv, counts.uav, counts.sampler}});
            previous = &program;
        }
        std::uint32_t vertex_reads = 0;
        const auto& vertex_info = programs[guest_hashes[1] ? 1 : 0].info;
        for (std::size_t i = 0; i < Pso::kNumVertexAttributes; ++i)
            if (vertex_info.loads.Generic(i))
                vertex_reads |= std::uint32_t{1} << i;
        for (size_t index = 0; index < state.attributes.size(); ++index) {
            if (!(vertex_reads & (u32{1} << index)) || regs.vertex_attrib_format[index].constant)
                continue;
            const auto& input = regs.vertex_attrib_format[index];
            const auto& attribute = state.attributes[index];
            const u32 size = static_cast<u32>(input.size.Value());
            if (!attribute.dxgi || attribute.swap_rb || attribute.scaled ||
                size == Pso::VSize::R16G16B16 || size == Pso::VSize::R8G8B8) {
                ReportUnsupported(fmt::format("vertex_attrib_format[{}] size={:x} type={} bgra={} "
                                              "requires vertex pulling/component conversion",
                                              index, size, static_cast<u32>(input.type.Value()),
                                              input.bgra != 0));
                guest_graphics.emplace(request, nullptr);
                return nullptr;
            }
        }
        Pso::ApplyVertexReads(state, vertex_reads);
        auto& pipeline = GetGraphics(state, topology.topology, stages);
        for (size_t index = 1; index < 6; ++index) {
            if (guest_hashes[index])
                pipeline.SetStageInfo(index - 1, &programs[index].info);
        }
        if (guest_hashes[0] && !guest_hashes[1])
            pipeline.SetStageInfo(0, &programs[0].info);
        guest_graphics.emplace(request, &pipeline);
        return &pipeline;
    } catch (const Shader::Exception& error) {
        ReportUnsupported(error.what());
    } catch (const std::exception& error) {
        Diagnostic(fmt::format("D3D12 graphics pipeline={:016x} what={}",
                               Pso::HashBytes(&request, sizeof(request)), error.what()));
        if (FAILED(context.Device()->GetDeviceRemovedReason()))
            context.ReportFailure(error);
    }
    guest_graphics.emplace(request, nullptr);
    return nullptr;
} catch (const std::exception& error) {
    ReportUnsupported(error.what());
    return nullptr;
} catch (...) {
    ReportUnsupported("unknown exception in graphics pipeline state");
    return nullptr;
}

ComputePipeline* PipelineCache::CurrentComputePipeline() try {
    if (!kepler_compute)
        return nullptr;
    std::scoped_lock lock{pipeline_mutex};
    SyncGuestHost();
    const auto* shader = ComputeShader();
    if (!shader)
        return nullptr;
    const auto& launch = kepler_compute->launch_description;
    const std::array<u32, 6> specialization{launch.block_dim_x,     launch.block_dim_y,
                                            launch.block_dim_z,     launch.shared_alloc,
                                            launch.local_pos_alloc, launch.local_crs_alloc};
    const u64 request =
        Pso::HashBytes(specialization.data(), sizeof(specialization), shader->unique_hash);
    if (const auto found = guest_compute.find(request); found != guest_compute.end())
        return found->second;
    try {
        const auto& qmd = kepler_compute->launch_description;
        VideoCommon::ComputeEnvironment env{*kepler_compute, *gpu_memory,
                                            kepler_compute->regs.code_loc.Address(),
                                            qmd.program_start};
        env.SetCachedSize(shader->size_bytes);
        TranslationState translation{context.Device()};
        Shader::Maxwell::Flow::CFG cfg{env, translation.flow, env.StartAddress()};
        auto program = Shader::Maxwell::TranslateProgram(translation.inst, translation.block, env,
                                                         cfg, translation.host);
        const auto source = Shader::Backend::HLSL::EmitHLSL(translation.profile, program);
        const auto counts = CountStageTables(program.info);
        auto& pipeline = GetCompute({Shader::Stage::Compute,
                                     shader->unique_hash,
                                     source,
                                     {counts.cbv, counts.srv, counts.uav, counts.sampler}});
        pipeline.info = program.info;
        guest_compute.emplace(request, &pipeline);
        return &pipeline;
    } catch (const std::exception& error) {
        Diagnostic(
            fmt::format("D3D12 compute HLSL/PSO pipeline={:016x} what={}", request, error.what()));
        if (FAILED(context.Device()->GetDeviceRemovedReason()))
            context.ReportFailure(error);
        guest_compute.emplace(request, nullptr);
        return nullptr;
    }
} catch (const std::exception& error) {
    ReportUnsupported(error.what());
    return nullptr;
} catch (...) {
    ReportUnsupported("unknown exception in compute pipeline state");
    return nullptr;
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (disk) {
        disk->StopWarmup();
        disk->Flush();
    }
    std::unique_lock lock{pipeline_mutex};
    context.Finish(); // Retire PSOs referenced by the previous title before replacing the maps.
    guest_graphics.clear();
    guest_compute.clear();
    graphics.clear();
    compute.clear();
    const auto identity = DiskPipelineCache::QueryIdentity(context.Device(), {3, 2, 3});
    if (!identity) {
        LOG_WARNING(Render, "D3D12 adapter identity unavailable; disk PSO reuse disabled");
        disk.reset();
        if (callback)
            callback(VideoCore::LoadCallbackStage::Complete, 0, 0);
        return;
    }
    // ShaderDir is under the frontend's LocalState path on UWP.
    disk = std::make_unique<DiskPipelineCache>(
        Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir), title_id, *identity,
        context.Device());
    disk->StartWarmup(
        [this](const DiskPipelineRecord& record, std::stop_token stop) {
            if (stop.stop_requested())
                return false;
            try {
                RecipeReader reader{record.payload};
                if (reader.U32() != 2)
                    return false;
                const u32 topology = reader.U32();
                Pso::GraphicsPsoState state{};
                if (record.kind == DiskRecordKind::Graphics) {
                    const auto raw = reader.Read(sizeof(state));
                    std::memcpy(&state, raw.data(), sizeof(state));
                } else if (record.kind != DiskRecordKind::Compute)
                    return false;
                const u32 count = reader.U32();
                if (!count || count > 6)
                    return false;
                std::vector<StageInput> stages;
                for (u32 i = 0; i < count; ++i) {
                    const auto stage = static_cast<Shader::Stage>(reader.U32());
                    if (static_cast<u32>(stage) > static_cast<u32>(Shader::Stage::Compute))
                        return false;
                    const u64 hash = reader.U64();
                    DescriptorCounts counts{reader.U32(), reader.U32(), reader.U32(), reader.U32()};
                    if (counts.cbv > 1024 || counts.srv > 16384 || counts.uav > 16384 ||
                        counts.sampler > 2048)
                        return false;
                    const u32 size = reader.U32();
                    const auto source = reader.Read(size);
                    stages.push_back({stage,
                                      hash,
                                      {reinterpret_cast<const char*>(source.data()), source.size()},
                                      counts});
                }
                if (!reader.Empty())
                    return false;
                if (record.kind == DiskRecordKind::Graphics)
                    GetGraphics(state, topology, stages);
                else if (stages.size() == 1 && stages[0].stage == Shader::Stage::Compute)
                    GetCompute(stages[0]);
                else
                    return false;
                return true;
            } catch (const std::exception& error) {
                LOG_WARNING(Render, "D3D12 disk PSO warmup rejected: {}", error.what());
                return false;
            }
        },
        [callback](const DiskWarmupProgress& progress) {
            if (callback)
                callback(progress.stage == DiskWarmupStage::Complete
                             ? VideoCore::LoadCallbackStage::Complete
                             : VideoCore::LoadCallbackStage::Build,
                         progress.completed, progress.total);
        },
        stop_loading);
    // The frontend callback captures loading-scope state. Join before that scope expires.
    lock.unlock();
    disk->WaitWarmup();
}
void PipelineCache::BindToChannel(s32 id) {
    VideoCommon::ShaderCache::BindToChannel(id);
    if (maxwell3d)
        maxwell3d->dirty.flags[VideoCommon::Dirty::Shaders] = true;
}
PipelineCache::~PipelineCache() {
    try {
        if (disk) {
            disk->StopWarmup();
            disk->Flush();
        }
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 pipeline cache teardown: {}", error.what());
    } catch (...) {
        LOG_ERROR(Render, "D3D12 pipeline cache teardown: unknown exception");
    }
}
void PipelineCache::FlushDiskCache() {
    if (disk)
        disk->Flush();
}

} // namespace D3D12
