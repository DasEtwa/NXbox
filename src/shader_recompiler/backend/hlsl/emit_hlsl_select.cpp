// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Backend::HLSL {
void EmitSelectU1(EmitContext& ctx, IR::Inst& inst, std::string_view cond,
                  std::string_view true_value, std::string_view false_value) {
    ctx.AddU1("{}={}?{}:{};", inst, cond, true_value, false_value);
}

void EmitSelectU8(EmitContext& ctx, IR::Inst& inst, std::string_view cond, std::string_view true_value, std::string_view false_value) {
    ctx.AddU32("{} = ({} ? {} : {}) & 0xffu;", inst, cond, true_value, false_value);
}

void EmitSelectU16(EmitContext& ctx, IR::Inst& inst, std::string_view cond, std::string_view true_value, std::string_view false_value) {
    ctx.AddU32("{} = ({} ? {} : {}) & 0xffffu;", inst, cond, true_value, false_value);
}

void EmitSelectU32(EmitContext& ctx, IR::Inst& inst, std::string_view cond,
                   std::string_view true_value, std::string_view false_value) {
    ctx.AddU32("{}={}?{}:{};", inst, cond, true_value, false_value);
}

void EmitSelectU64(EmitContext& ctx, IR::Inst& inst, std::string_view cond,
                   std::string_view true_value, std::string_view false_value) {
    ctx.AddU64("{}={}?{}:{};", inst, cond, true_value, false_value);
}

void EmitSelectF16(EmitContext& ctx, IR::Inst& inst, std::string_view cond, std::string_view true_value, std::string_view false_value) {
    ctx.AddF32("{} = nx_half({} ? {} : {});", inst, cond, true_value, false_value);
}

void EmitSelectF32(EmitContext& ctx, IR::Inst& inst, std::string_view cond,
                   std::string_view true_value, std::string_view false_value) {
    ctx.AddF32("{}={}?{}:{};", inst, cond, true_value, false_value);
}

void EmitSelectF64(EmitContext& ctx, IR::Inst& inst, std::string_view cond,
                   std::string_view true_value, std::string_view false_value) {
    ctx.AddF64("{}={}?{}:{};", inst, cond, true_value, false_value);
}

} // namespace Shader::Backend::HLSL
