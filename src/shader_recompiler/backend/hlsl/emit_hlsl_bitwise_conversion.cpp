// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/profile.h"

namespace Shader::Backend::HLSL {
void EmitIdentity(EmitContext&, IR::Inst&, const IR::Value&) {}

void EmitConditionRef(EmitContext& ctx, IR::Inst& inst, const IR::Value& value) {
    const auto ret{ctx.var_alloc.Define(inst, HlslVarType::U1)};
    const auto input{ctx.var_alloc.Consume(value)};
    if (ret != input) {
        ctx.Add("{}={};", ret, input);
    }
}

void EmitBitCastU16F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = f32tof16({});", inst, value);
}

void EmitBitCastU32F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{}=asuint({});", inst, value);
}

void EmitBitCastU64F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{}=doubleBitsToUint64({});", inst, value);
}

void EmitBitCastF16U16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = f16tof32({} & 0xffffu);", inst, value);
}

void EmitBitCastF32U32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=asfloat({});", inst, value);
}

void EmitBitCastF64U64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=uint64BitsToDouble({});", inst, value);
}

void EmitPackUint2x32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{}=packUint2x32({});", inst, value);
}

void EmitUnpackUint2x32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32x2("{}=unpackUint2x32({});", inst, value);
}

void EmitPackFloat2x16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{}=packFloat2x16({});", inst, value);
}

void EmitUnpackFloat2x16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF16x2("{}=unpackFloat2x16({});", inst, value);
}

void EmitPackHalf2x16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{}=packHalf2x16({});", inst, value);
}

void EmitUnpackHalf2x16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32x2("{}=unpackHalf2x16({});", inst, value);
}

void EmitPackDouble2x32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=packDouble2x32({});", inst, value);
}

void EmitUnpackDouble2x32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32x2("{}=unpackDouble2x32({});", inst, value);
}

} // namespace Shader::Backend::HLSL
