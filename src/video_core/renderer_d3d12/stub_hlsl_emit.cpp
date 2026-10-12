// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/stub_hlsl_emit.h"

#if __has_include("shader_recompiler/backend/hlsl/emit_hlsl.h")
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#define NXBOX_HAVE_HLSL_BACKEND 1
#endif

namespace D3D12 {

std::string_view StubHlsl(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::Fragment:
        return "float4 main() : SV_Target0 { return float4(1, 0, 1, 1); }\n";
    case Shader::Stage::Compute:
        return "[numthreads(1, 1, 1)] void main() {}\n";
    case Shader::Stage::Geometry:
        return "[maxvertexcount(3)] void main(triangle float4 p[3] : SV_Position, "
               "inout TriangleStream<float4> o) {}\n";
    case Shader::Stage::TessellationControl:
    case Shader::Stage::TessellationEval:
        // No sensible trivial hull/domain pair; the cache never builds tessellation with this stub.
        return {};
    default:
        return "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n";
    }
}

std::string TranslateToHlsl(const Shader::Profile& profile, const Shader::RuntimeInfo& runtime_info,
                            Shader::IR::Program& program, Shader::Backend::Bindings& bindings) {
#ifdef NXBOX_HAVE_HLSL_BACKEND
    return Shader::Backend::HLSL::EmitHLSL(profile, runtime_info, program, bindings);
#else
    // TODO(integration): STUB, ignores the IR. Draws nothing meaningful; only lets the PSO
    // path compile and link until the real HLSL backend is merged.
    (void)profile;
    (void)runtime_info;
    (void)bindings;
    return std::string{StubHlsl(program.stage)};
#endif
}

} // namespace D3D12
