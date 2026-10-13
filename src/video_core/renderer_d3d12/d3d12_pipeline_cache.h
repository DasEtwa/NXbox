// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <unordered_set>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "shader_recompiler/stage.h"
#include "video_core/renderer_d3d12/d3d12_async_pso.h"
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_dxc.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_iface.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/shader_cache.h"

namespace Tegra::Engines {
class Maxwell3D;
}

namespace D3D12 {

// One translated stage, as produced by EmitHLSL from the stage's
// IR::Program. `hash` is the guest shader's unique hash (same value Vulkan keys on).
struct StageInput {
    Shader::Stage stage;
    u64 hash;
    std::string_view hlsl;
    DescriptorCounts counts; // CountDescriptors(program.info)
    const Shader::RuntimeInfo* runtime{};
};

struct GraphicsPipelineKey {
    std::array<u64, 6> stage_hashes; // indexed by Shader::Stage value (VertexB..Fragment)
    Pso::GraphicsPsoState state;
    bool operator==(const GraphicsPipelineKey&) const noexcept;
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineKey>);

struct GraphicsPipelineKeyHash {
    size_t operator()(const GraphicsPipelineKey& key) const noexcept {
        return static_cast<size_t>(Pso::HashBytes(&key, sizeof(key)));
    }
};

// Reads the Maxwell registers into the packed PSO key (blend, raster, depth-stencil, vertex
// input and topology). Actual framebuffer formats are applied by the draw path.
void FillGraphicsState(Tegra::Engines::Maxwell3D& maxwell3d, Pso::GraphicsPsoState& state);

// Request snapshots belong to the GPU thread; asynchronous jobs own their compilation inputs.
class PipelineCache final : public IPipelineCache, public VideoCommon::ShaderCache {
public:
    PipelineCache(Context& context_, Tegra::MaxwellDeviceMemoryManager& memory)
        : VideoCommon::ShaderCache{memory}, context{context_} {}
    ~PipelineCache() override;
    void BindToChannel(s32 id) override;
    IGraphicsPipeline* CurrentGraphicsPipeline(const Pso::FramebufferFormats& formats) override;
    ComputePipeline* CurrentComputePipeline();
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);
    void FlushDiskCache();
    std::string ProfileSummary();

    // Returns the pipeline for (state, stages), compiling HLSL->DXIL and creating the PSO on a
    // miss. `stages` must hold one entry per active stage.
    GraphicsPipeline& GetGraphics(const Pso::GraphicsPsoState& state, u32 d3d_topology,
                                  std::span<const StageInput> stages);
    ComputePipeline& GetCompute(const StageInput& stage);

    // Shared guest root signature, keyed by descriptor counts and CBV layout choice.
    ID3D12RootSignature* GetRootSignature(DescriptorCounts counts);

private:
    struct CountsLess {
        bool operator()(const std::pair<DescriptorCounts, bool>& a,
                        const std::pair<DescriptorCounts, bool>& b) const {
            return std::tie(a.first.cbv, a.first.srv, a.first.uav, a.first.sampler, a.second) <
                   std::tie(b.first.cbv, b.first.srv, b.first.uav, b.first.sampler, b.second);
        }
    };

    struct AsyncResult {
        std::shared_ptr<GraphicsPipeline> graphics;
        std::shared_ptr<ComputePipeline> compute;
    };
    using AsyncCache =
        AsyncPsoWorkerPool<GraphicsPipelineKey, AsyncResult, GraphicsPipelineKeyHash>;
    std::unique_ptr<AsyncCache> async;
    std::recursive_mutex pipeline_mutex;
    Context& context;
    std::array<u64, 6> guest_hashes{};
    DxcCache dxc{&context.Profile()};
    std::unique_ptr<DiskPipelineCache> disk;
    std::map<std::pair<DescriptorCounts, bool>, Microsoft::WRL::ComPtr<ID3D12RootSignature>,
             CountsLess>
        root_signatures;
    std::unordered_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>,
                       GraphicsPipelineKeyHash>
        graphics;
    std::unordered_map<GraphicsPipelineKey, std::string, GraphicsPipelineKeyHash> failed_graphics;
    std::unordered_map<u64, std::unique_ptr<ComputePipeline>> compute;
    std::unordered_map<GraphicsPipelineKey, GraphicsPipeline*, GraphicsPipelineKeyHash>
        guest_graphics;
    std::unordered_map<u64, ComputePipeline*> guest_compute;
    // Requests whose native compile never returned on this console, kept across boots so they
    // fail fast instead of tying up (and spinning) another compiler thread.
    std::unordered_set<u64> stalled_requests;
    std::filesystem::path stalled_path;
    bool CheckStalled(const GraphicsPipelineKey& key, u64 hash);
};

} // namespace D3D12
