// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>

#include <d3d12.h>
#include <wrl/client.h>

#include "shader_recompiler/stage.h"
#include "video_core/renderer_d3d12/d3d12_dxc.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_iface.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"

namespace D3D12 {

struct StageProgram {
    Shader::Stage stage;
    DxilBlob dxil;
};

class GraphicsPipeline final : public IGraphicsPipeline {
public:
    // Throws on PSO creation failure. `programs` holds one entry per active stage.
    GraphicsPipeline(ID3D12Device* device,
                     Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature,
                     const Pso::GraphicsPsoState& state, u32 d3d_topology,
                     std::span<const StageProgram> programs);

    // IGraphicsPipeline. A front-and-back cull draws nothing and D3D12 cannot express it, so
    // the descriptor/draw path sees "no PSO" and skips the draw.
    ID3D12PipelineState* PipelineState() const override {
        return CullsEverything() ? nullptr : pso.Get();
    }
    ID3D12RootSignature* RootSignature() const override {
        return root_signature.Get();
    }
    D3D_PRIMITIVE_TOPOLOGY Topology() const override {
        return static_cast<D3D_PRIMITIVE_TOPOLOGY>(topology);
    }
    const Shader::Info* StageInfo(size_t stage) const override {
        return stage_infos[stage];
    }
    // The Info must outlive the pipeline (it lives with the translated program).
    void SetStageInfo(size_t stage, const Shader::Info* info) {
        stage_infos[stage] = info;
    }
    // RootConstants() keeps the iface default (render area + y_flip in words 0..2) until the
    // HLSL backend defines hlsl_root_constants.h; then override it here.
    const Pso::GraphicsPsoState& State() const {
        return state;
    }
    // Cull mode FrontAndBack: D3D12 cannot express it, the rasterizer must skip the draw.
    bool CullsEverything() const {
        return state.cull_all != 0;
    }

private:
    Pso::GraphicsPsoState state;
    u32 topology{};
    std::array<const Shader::Info*, NUM_GRAPHICS_STAGES> stage_infos{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

} // namespace D3D12
