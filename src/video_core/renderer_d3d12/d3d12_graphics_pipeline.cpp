// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include "shader_recompiler/backend/hlsl/hlsl_vertex_semantic.h"

#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"

namespace D3D12 {
namespace {
// Each stream subobject has pointer alignment, as required by CreatePipelineState.
// Use a stream only for state the legacy graphics descriptor cannot represent.
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) StreamObject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
    T value{};
};
struct DepthBoundsStream {
    StreamObject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
    StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
    StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS> hs;
    StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS> ds;
    StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS> gs;
    StreamObject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    StreamObject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> mask;
    StreamObject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
    StreamObject<D3D12_DEPTH_STENCIL_DESC1, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1>
        depth;
    StreamObject<D3D12_INPUT_LAYOUT_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT> input;
    StreamObject<D3D12_PRIMITIVE_TOPOLOGY_TYPE,
                 D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY>
        topology;
    StreamObject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS>
        colors;
    StreamObject<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT>
        depth_format;
    StreamObject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> samples;
};
void CreateDepthBoundsPipeline(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                               ComPtr<ID3D12PipelineState>& pso, u64 key) {
    ComPtr<ID3D12Device2> device2;
    Check(device->QueryInterface(IID_PPV_ARGS(&device2)), "D3D12 depth-bounds device interface",
          device);
    DepthBoundsStream stream{};
    stream.root.value = desc.pRootSignature;
    stream.vs.value = desc.VS;
    stream.ps.value = desc.PS;
    stream.hs.value = desc.HS;
    stream.ds.value = desc.DS;
    stream.gs.value = desc.GS;
    stream.blend.value = desc.BlendState;
    stream.mask.value = desc.SampleMask;
    stream.raster.value = desc.RasterizerState;
    const auto& depth = desc.DepthStencilState;
    stream.depth.value = {depth.DepthEnable,   depth.DepthWriteMask,  depth.DepthFunc,
                          depth.StencilEnable, depth.StencilReadMask, depth.StencilWriteMask,
                          depth.FrontFace,     depth.BackFace,        TRUE};
    stream.input.value = desc.InputLayout;
    stream.topology.value = desc.PrimitiveTopologyType;
    std::copy(std::begin(desc.RTVFormats), std::end(desc.RTVFormats),
              std::begin(stream.colors.value.RTFormats));
    stream.colors.value.NumRenderTargets = desc.NumRenderTargets;
    stream.depth_format.value = desc.DSVFormat;
    stream.samples.value = desc.SampleDesc;
    const D3D12_PIPELINE_STATE_STREAM_DESC description{sizeof(stream), &stream};
    CheckPipeline(device2->CreatePipelineState(&description, IID_PPV_ARGS(&pso)),
                  "CreatePipelineState(depth bounds)", device, key);
}

D3D12_DEPTH_STENCILOP_DESC StencilFace(const Pso::StencilFaceState& face) {
    return {
        static_cast<D3D12_STENCIL_OP>(face.fail), static_cast<D3D12_STENCIL_OP>(face.depth_fail),
        static_cast<D3D12_STENCIL_OP>(face.pass), static_cast<D3D12_COMPARISON_FUNC>(face.func)};
}

D3D12_SHADER_BYTECODE Bytecode(const StageProgram& program) {
    return {program.dxil->data(), program.dxil->size()};
}
} // namespace

GraphicsPipeline::GraphicsPipeline(ID3D12Device* device, ComPtr<ID3D12RootSignature> signature,
                                   const Pso::GraphicsPsoState& state_, u32 d3d_topology,
                                   std::span<const StageProgram> programs, DiskPipelineCache* disk,
                                   u64 key, std::span<const u8> recipe)
    : state{state_}, topology{d3d_topology}, root_signature{std::move(signature)} {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature.Get();
    for (const auto& program : programs) {
        switch (program.stage) {
        case Shader::Stage::VertexA:
        case Shader::Stage::VertexB:
            desc.VS = Bytecode(program);
            break;
        case Shader::Stage::TessellationControl:
            desc.HS = Bytecode(program);
            break;
        case Shader::Stage::TessellationEval:
            desc.DS = Bytecode(program);
            break;
        case Shader::Stage::Geometry:
            desc.GS = Bytecode(program);
            break;
        case Shader::Stage::Fragment:
            desc.PS = Bytecode(program);
            break;
        default:
            break;
        }
    }

    // Blend: IndependentBlendEnable must be set for per-target state to take effect.
    desc.BlendState.IndependentBlendEnable = state.independent_blend != 0;
    desc.BlendState.AlphaToCoverageEnable = state.alpha_to_coverage != 0;
    for (size_t i = 0; i < Pso::kNumRenderTargets; ++i) {
        const auto& b = state.blend[i];
        auto& rt = desc.BlendState.RenderTarget[i];
        rt.BlendEnable = b.enable != 0;
        rt.LogicOpEnable = state.logic_enable != 0;
        rt.SrcBlend = static_cast<D3D12_BLEND>(b.src);
        rt.DestBlend = static_cast<D3D12_BLEND>(b.dst);
        rt.BlendOp = static_cast<D3D12_BLEND_OP>(b.op);
        rt.SrcBlendAlpha = static_cast<D3D12_BLEND>(b.src_alpha);
        rt.DestBlendAlpha = static_cast<D3D12_BLEND>(b.dst_alpha);
        rt.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(b.op_alpha);
        rt.LogicOp = static_cast<D3D12_LOGIC_OP>(state.logic_op);
        rt.RenderTargetWriteMask = b.write_mask; // R=1 G=2 B=4 A=8, same as D3D12
    }
    desc.SampleMask = 0xFFFFFFFFu;

    desc.RasterizerState.FillMode = static_cast<D3D12_FILL_MODE>(state.fill_mode);
    desc.RasterizerState.CullMode = static_cast<D3D12_CULL_MODE>(state.cull_mode);
    desc.RasterizerState.FrontCounterClockwise = state.front_ccw != 0;
    desc.RasterizerState.DepthBias = state.depth_bias;
    std::memcpy(&desc.RasterizerState.DepthBiasClamp, &state.bias_clamp_bits, sizeof(float));
    std::memcpy(&desc.RasterizerState.SlopeScaledDepthBias, &state.slope_scaled_bias_bits,
                sizeof(float));
    desc.RasterizerState.DepthClipEnable = state.depth_clip != 0;
    desc.RasterizerState.AntialiasedLineEnable = state.smooth_lines != 0;
    desc.RasterizerState.ConservativeRaster = state.conservative_raster
                                                  ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                                  : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    auto& ds = desc.DepthStencilState;
    ds.DepthEnable = state.depth_enable != 0;
    ds.DepthWriteMask =
        state.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(state.depth_func);
    ds.StencilEnable = state.stencil_enable != 0;
    ds.StencilReadMask = state.stencil_read_mask;
    ds.StencilWriteMask = state.stencil_write_mask;
    ds.FrontFace = StencilFace(state.front);
    ds.BackFace = StencilFace(state.back);

    // Match the HLSL generic input signature exactly.
    std::array<D3D12_INPUT_ELEMENT_DESC, Pso::kNumVertexAttributes> elements{};
    UINT element_count = 0;
    for (UINT i = 0; i < Pso::kNumVertexAttributes; ++i) {
        const auto& a = state.attributes[i];
        if (!a.enabled || a.dxgi == 0) {
            continue;
        }
        const auto semantic = Shader::Backend::HLSL::GenericSemantic(i);
        elements[element_count++] = {semantic.name,
                                     semantic.index,
                                     static_cast<DXGI_FORMAT>(a.dxgi),
                                     a.slot,
                                     a.offset,
                                     a.instanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                 : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                                     a.instanced ? static_cast<UINT>(a.divisor) : 0u};
    }
    desc.InputLayout = {elements.data(), element_count};
    desc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(state.topology_type);
    desc.NumRenderTargets = state.num_render_targets;
    for (size_t i = 0; i < Pso::kNumRenderTargets; ++i) {
        desc.RTVFormats[i] = static_cast<DXGI_FORMAT>(state.rtv_formats[i]);
    }
    desc.DSVFormat = static_cast<DXGI_FORMAT>(state.dsv_format);
    desc.SampleDesc = {state.sample_count ? state.sample_count : 1u, 0};
    desc.RasterizerState.MultisampleEnable = desc.SampleDesc.Count > 1;

    if (state.depth_bounds) {
        // Legacy disk PSO descriptors cannot encode DepthBoundsTestEnable. These
        // PSOs remain memory-cached; DXIL cache reuse is unchanged.
        CreateDepthBoundsPipeline(device, desc, pso, key);
        return;
    }
    if (disk) {
        pso = disk->LoadOrCreateGraphics(key, desc, recipe);
        if (!pso)
            throw std::runtime_error("D3D12 disk graphics PSO creation failed");
        return;
    }
    CheckPipeline(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)),
                  "CreateGraphicsPipelineState", device, key);
}

} // namespace D3D12
