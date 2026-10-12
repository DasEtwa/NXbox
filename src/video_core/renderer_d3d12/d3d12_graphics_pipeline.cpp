// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstring>
#include <vector>
#include "shader_recompiler/backend/hlsl/hlsl_vertex_semantic.h"

#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"

namespace D3D12 {
namespace {
D3D12_DEPTH_STENCILOP_DESC StencilFace(const Pso::StencilFaceState& face) {
    return {static_cast<D3D12_STENCIL_OP>(face.fail), static_cast<D3D12_STENCIL_OP>(face.depth_fail),
            static_cast<D3D12_STENCIL_OP>(face.pass),
            static_cast<D3D12_COMPARISON_FUNC>(face.func)};
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
    for (size_t i = 0; i < Pso::kNumRenderTargets; ++i) {
        const auto& b = state.blend[i];
        auto& rt = desc.BlendState.RenderTarget[i];
        rt.BlendEnable = b.enable != 0;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = static_cast<D3D12_BLEND>(b.src);
        rt.DestBlend = static_cast<D3D12_BLEND>(b.dst);
        rt.BlendOp = static_cast<D3D12_BLEND_OP>(b.op);
        rt.SrcBlendAlpha = static_cast<D3D12_BLEND>(b.src_alpha);
        rt.DestBlendAlpha = static_cast<D3D12_BLEND>(b.dst_alpha);
        rt.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(b.op_alpha);
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
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

    auto& ds = desc.DepthStencilState;
    ds.DepthEnable = state.depth_enable != 0;
    ds.DepthWriteMask = state.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
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
                                     a.instanced ? 1u : 0u};
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
