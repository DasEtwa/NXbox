// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "video_core/renderer_d3d12/d3d12_dxc.h"

namespace D3D12 {

class ComputePipeline {
public:
    ComputePipeline(ID3D12Device* device, Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature,
                    const DxilBlob& dxil);

    ID3D12PipelineState* PipelineState() const {
        return pso.Get();
    }
    ID3D12RootSignature* RootSignature() const {
        return root_signature.Get();
    }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

} // namespace D3D12
