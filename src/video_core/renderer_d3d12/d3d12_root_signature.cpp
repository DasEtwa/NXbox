// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_build_stats.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"

namespace D3D12 {

DescriptorCounts CountDescriptors(const Shader::Info& info) {
    DescriptorCounts counts;
    counts.cbv = Shader::NumDescriptors(info.constant_buffer_descriptors);
    counts.srv = Shader::NumDescriptors(info.texture_descriptors) +
                 Shader::NumDescriptors(info.texture_buffer_descriptors);
    counts.sampler = Shader::NumDescriptors(info.texture_descriptors);
    counts.uav = Shader::NumDescriptors(info.storage_buffers_descriptors) +
                 Shader::NumDescriptors(info.image_descriptors) +
                 Shader::NumDescriptors(info.image_buffer_descriptors);
    return counts;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, DescriptorCounts counts,
                                                bool root_cbvs) {
    const PsoBuildTimer timer{&PsoBuildStats::root_us};
    root_cbvs = root_cbvs && UseRootCbvs(counts.cbv);
    const auto range = [](D3D12_DESCRIPTOR_RANGE_TYPE type, u32 count) {
        return D3D12_DESCRIPTOR_RANGE{type, std::max(count, 1u), 0, 0,
                                      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    };
    std::array ranges{
        range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, counts.cbv),
        range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, counts.srv),
        range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, counts.uav),
        range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, counts.sampler),
    };
    std::vector<D3D12_ROOT_PARAMETER> params(root_cbvs ? kFirstRootCbv + counts.cbv
                                                       : RootParameterCount);
    params[RootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[RootConstants].Constants = {0, 1, kRootConstantDwords}; // b0, space1
    params[RootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (u32 i = root_cbvs ? 1u : 0u; i < ranges.size(); ++i) {
        auto& p = params[RootCbvTable + i - (root_cbvs ? 1u : 0u)];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = {1, &ranges[i]};
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // EmitHLSL advances uniform_buffer across stages: b0..bN in space0.
    if (root_cbvs) {
        for (u32 i = 0; i < counts.cbv; ++i) {
            auto& p = params[kFirstRootCbv + i];
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p.Descriptor = {i, 0};
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(params.size());
    desc.pParameters = params.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                 D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;

    ComPtr<ID3DBlob> blob, error;
    const HRESULT hr =
        D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
    if (FAILED(hr) && error) {
        LOG_ERROR(Render, "D3D12 root signature: {}",
                  std::string_view{static_cast<const char*>(error->GetBufferPointer()),
                                   error->GetBufferSize()});
    }
    Check(hr, "D3D12SerializeRootSignature", device);
    ComPtr<ID3D12RootSignature> signature;
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&signature)),
          "CreateRootSignature", device);
    return signature;
}

} // namespace D3D12
