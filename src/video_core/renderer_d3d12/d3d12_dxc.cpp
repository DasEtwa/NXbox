// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_dxc.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"

namespace D3D12 {

const wchar_t* StageTarget(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexA:
    case Shader::Stage::VertexB:
        return L"vs_6_0";
    case Shader::Stage::TessellationControl:
        return L"hs_6_0";
    case Shader::Stage::TessellationEval:
        return L"ds_6_0";
    case Shader::Stage::Geometry:
        return L"gs_6_0";
    case Shader::Stage::Fragment:
        return L"ps_6_0";
    case Shader::Stage::Compute:
    default:
        return L"cs_6_0";
    }
}

DxilBlob DxcCache::Compile(std::string_view hlsl, Shader::Stage stage) {
    const wchar_t* target = StageTarget(stage);
    u64 hash = Pso::HashBytes(hlsl.data(), hlsl.size());
    const auto stage_value = static_cast<u32>(stage);
    hash = Pso::HashBytes(&stage_value, sizeof(stage_value), hash);
    const auto [begin, end] = entries.equal_range(hash);
    for (auto it = begin; it != end; ++it) {
        if (it->second.target == target && it->second.source == hlsl) {
            return it->second.dxil;
        }
    }
    auto dxil = std::make_shared<const std::vector<u8>>(compiler.Compile(hlsl, L"main", target));
    entries.emplace(hash, Entry{std::string{hlsl}, target, dxil});
    return dxil;
}

} // namespace D3D12
