// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>

#include <d3d12.h>
#include <wrl/client.h>

#include "shader_recompiler/stage.h"
#include "video_core/renderer_d3d12/d3d12_dxc.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"

namespace D3D12 {

struct StageProgram {
    Shader::Stage stage;
    DxilBlob dxil;
};

class GraphicsPipeline {
public:
    // Throws on PSO creation failure. `programs` holds one entry per active stage.
    GraphicsPipeline(ID3D12Device* device,
                     Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature,
                     const Pso::GraphicsPsoState& state, std::span<const StageProgram> programs);

    ID3D12PipelineState* PipelineState() const {
        return pso.Get();
    }
    ID3D12RootSignature* RootSignature() const {
        return root_signature.Get();
    }
    const Pso::GraphicsPsoState& State() const {
        return state;
    }
    // Cull mode FrontAndBack: D3D12 cannot express it, the rasterizer must skip the draw.
    bool CullsEverything() const {
        return state.cull_all != 0;
    }

private:
    Pso::GraphicsPsoState state;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

} // namespace D3D12
