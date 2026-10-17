// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"

namespace D3D12 {

// Legacy table layout, also used by internal passes. Guest root-CBV layouts omit
// the CBV table: SRV/UAV/sampler occupy 1..3, followed by CBVs starting at 4.
enum RootParameter : u32 {
    RootConstants = 0,
    RootCbvTable = 1,
    RootSrvTable = 2,
    RootUavTable = 3,
    RootSamplerTable = 4,
    RootParameterCount = 5,
};
inline constexpr u32 kRootConstantDwords = 16;
inline constexpr u32 kFirstRootCbv = 4;
inline constexpr u32 kRootSignatureDwordLimit = 64;

// Constants cost 16 DWORDs, each remaining table one, and each root CBV two.
constexpr bool UseRootCbvs(u32 count) {
    return count <= (kRootSignatureDwordLimit - kRootConstantDwords - 3) / 2;
}

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

// Builds an uncached signature. Empty tables still have one null descriptor.
// Guest pipelines request root CBVs; oversized layouts fall back to four tables.
Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                                DescriptorCounts counts,
                                                                bool root_cbvs = false);

} // namespace D3D12
