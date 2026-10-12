// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"

namespace D3D12 {

// Root parameter indices, fixed by P3_CONTRACT.md. The descriptor allocator fills tables 1..4
// in exactly this order; table 0 is the 16-dword NxboxRootConstants block (b0, space1).
enum RootParameter : u32 {
    RootConstants = 0,
    RootCbvTable = 1,
    RootSrvTable = 2,
    RootUavTable = 3,
    RootSamplerTable = 4,
    RootParameterCount = 5,
};
inline constexpr u32 kRootConstantDwords = 16;

// Number of descriptors in each table (all stages of a pipeline summed, in binding order).
struct DescriptorCounts {
    u32 cbv{};
    u32 srv{};
    u32 uav{};
    u32 sampler{};
    bool operator==(const DescriptorCounts&) const = default;

    DescriptorCounts& operator+=(const DescriptorCounts& rhs) {
        cbv += rhs.cbv;
        srv += rhs.srv;
        uav += rhs.uav;
        sampler += rhs.sampler;
        return *this;
    }
};

// cbv = constant buffers; srv = textures then texel buffers; sampler = textures;
// uav = storage buffers then images then image buffers (the contract order).
DescriptorCounts CountDescriptors(const Shader::Info& info);

// Builds (uncached) the root signature for the given table sizes. A table with 0 descriptors
// still gets one range so the parameter index stays fixed; it is simply never bound.
Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                                DescriptorCounts counts);

} // namespace D3D12
