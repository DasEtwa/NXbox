// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::HLSL {

[[nodiscard]] u32 RequiredShaderModel(const IR::Program& program);

std::string EmitHLSL(const Profile& profile, const RuntimeInfo& runtime_info,
                                   IR::Program& program, Bindings& bindings);

[[nodiscard]] inline std::string EmitHLSL(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitHLSL(profile, {}, program, binding);
}

} // namespace Shader::Backend::HLSL
