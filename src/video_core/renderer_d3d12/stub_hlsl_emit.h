// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace D3D12 {

// Trivial passthrough shader for a stage (empty for tessellation). Used by the pipeline cache
// until guest shaders are translated.
[[nodiscard]] std::string_view StubHlsl(Shader::Stage stage);

// Shader::Backend::HLSL::EmitHLSL as declared by P3_CONTRACT.md. When the HLSL worker's
// src/shader_recompiler/backend/hlsl/emit_hlsl.h exists, this forwards to it; otherwise it
// returns a trivial passthrough shader for the stage.
// TODO(integration): once backend/hlsl lands, delete stub_hlsl_emit.cpp (the forwarding path
// in this header keeps working unchanged).
[[nodiscard]] std::string TranslateToHlsl(const Shader::Profile& profile,
                                          const Shader::RuntimeInfo& runtime_info,
                                          Shader::IR::Program& program,
                                          Shader::Backend::Bindings& bindings);

} // namespace D3D12
