// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_query_helpers.h"

namespace D3D12 {
// Integration: the graphics PSO uses Declaration(). Draw/HandleTransformFeedback
// passes the last vertex stage's RuntimeInfo (xfb_varyings are there, not Info)
// to Configure(), then routes buffer-cache SO bindings to BindBuffer(). Call Bind()
// after command-list resets and Unbind() when feedback is disabled. Guest buffers
// must be obtained with MarkAsWritten so CPU downloads observe stream output.
// The HLSL worker may supply a semantic resolver matching its output signature;
// the default accepts SV_Position and TEXCOORD0..31 only. Unsupported legacy
// outputs fail explicitly. The PSO root signature needs ALLOW_STREAM_OUTPUT.
class TransformFeedback {
public:
    struct Semantic {
        std::string name;
        u32 index{};
    };
    using SemanticResolver = std::function<Semantic(u32 attribute)>;
    explicit TransformFeedback(Context& context);
    void Configure(const Shader::RuntimeInfo& info, SemanticResolver resolver = {});
    D3D12_STREAM_OUTPUT_DESC Declaration(bool rasterize = true, u32 rasterized_stream = 0) const;
    void BindBuffer(u32 slot, ResourcePtr buffer, u64 offset, u64 size,
                    std::optional<u64> filled_size = std::nullopt);
    void Bind(bool cache_draw_state = false);
    void Unbind(bool reset_cursor = false, bool cache_draw_state = false);
    u32 StreamMask() const;
    u64 ReadFilledSize(u32 stream);
    void CopyFilledSize(u32 stream, const ResourcePtr& destination);

private:
    void EnsureCounters();
    Context& context;
    StreamOutputLayout layout;
    std::deque<std::string> semantics;
    std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
    std::array<ResourcePtr, 4> buffers;
    std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW, 4> views{};
    ResourcePtr counters;
    StagingRing upload;
    StagingRing readback;
    std::array<bool, 4> reset_pending{};
    bool bound{};
};
} // namespace D3D12
