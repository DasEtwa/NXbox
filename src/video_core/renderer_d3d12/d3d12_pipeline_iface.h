// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <d3d12.h>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"

// Minimal seam between the descriptor/draw path (this worker) and the PSO worker.
// d3d12_pipeline_cache.h / d3d12_graphics_pipeline.h must derive from these:
//   class GraphicsPipeline : public IGraphicsPipeline { ... };
//   class PipelineCache    : public IPipelineCache    { ... };
// and the integration code calls RasterizerD3D12::SetPipelineCache(&pipeline_cache).
namespace D3D12 {
// Stage order matches Maxwell: 0 VertexB, 1 TessControl, 2 TessEval, 3 Geometry, 4 Fragment.
inline constexpr size_t NUM_GRAPHICS_STAGES = 5;

class IGraphicsPipeline {
public:
    virtual ~IGraphicsPipeline() = default;
    // Root signature layout (contract): 0 root constants b0/space1 (16 dwords),
    // 1 CBV table, 2 SRV table, 3 UAV table, 4 sampler table; visibility ALL.
    virtual ID3D12RootSignature* RootSignature() const = 0;
    // nullptr while the PSO is still compiling: the draw is skipped.
    virtual ID3D12PipelineState* PipelineState() const = 0;
    virtual D3D_PRIMITIVE_TOPOLOGY Topology() const = 0;
    // nullptr for a disabled stage. The Info must outlive the pipeline.
    virtual const Shader::Info* StageInfo(size_t stage) const = 0;
    // Dwords for the NxboxRootConstants block (hlsl_root_constants.h layout). Called every
    // draw. y_flip is true when the Maxwell viewport has a negative height: D3D12 cannot
    // express that, so the vertex shader must negate clip-space Y.
    struct DrawInputs {
        u32 render_area_width{};
        u32 render_area_height{};
        bool y_flip{};
    };
    virtual std::array<u32, 16> RootConstants(const DrawInputs& inputs) const {
        std::array<u32, 16> words{};
        words[0] = inputs.render_area_width;
        words[1] = inputs.render_area_height;
        words[2] = inputs.y_flip ? 1u : 0u;
        return words;
    }
};

class IPipelineCache {
public:
    virtual ~IPipelineCache() = default;
    // Builds or looks up the pipeline for the current Maxwell3D state. nullptr skips the draw.
    virtual IGraphicsPipeline* CurrentGraphicsPipeline() = 0;
};

// Output slot i of the fragment shader writes color target draw_buffers[i]; the rasterizer
// binds OM slots 0..regs.rt_control.count-1 (null RTV where no target exists). The PSO's
// RTVFormats must follow the same slot order.
// Root tables are never empty: declare every table with TableSize(count) (>= 1) descriptors.
// Tables are shared by all stages in stage order 0..4 (registers keep counting across stages).

inline constexpr u32 TableSize(u32 count) {
    return count ? count : 1;
}

// Number of descriptors one stage consumes in each root table (derived from Shader::Info,
// identical to what the PSO worker must declare in the root signature ranges).
struct StageTableCounts {
    u32 cbv{};
    u32 srv{};
    u32 uav{};
    u32 sampler{};
};
inline StageTableCounts CountStageTables(const Shader::Info& info) {
    return {
        .cbv = Shader::NumDescriptors(info.constant_buffer_descriptors),
        .srv = Shader::NumDescriptors(info.texture_descriptors) +
               Shader::NumDescriptors(info.texture_buffer_descriptors),
        .uav = Shader::NumDescriptors(info.storage_buffers_descriptors) +
               Shader::NumDescriptors(info.image_buffer_descriptors) +
               Shader::NumDescriptors(info.image_descriptors),
        .sampler = Shader::NumDescriptors(info.texture_descriptors),
    };
}
} // namespace D3D12
