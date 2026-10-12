// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_context.h"

namespace D3D12 {

ComputePipeline::ComputePipeline(ID3D12Device* device, ComPtr<ID3D12RootSignature> signature,
                                 const DxilBlob& dxil, DiskPipelineCache* disk, u64 key,
                                 std::span<const u8> recipe)
    : root_signature{std::move(signature)} {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature.Get();
    desc.CS = {dxil->data(), dxil->size()};
    if (disk) {
        pso = disk->LoadOrCreateCompute(key, desc, recipe);
        if (!pso)
            throw std::runtime_error("D3D12 disk compute PSO creation failed");
        return;
    }
    Check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)),
          "CreateComputePipelineState");
}

} // namespace D3D12
