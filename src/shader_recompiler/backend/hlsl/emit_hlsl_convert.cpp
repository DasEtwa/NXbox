// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Backend::HLSL {
void EmitConvertS16F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(int({}), 0, 16);", inst, value);
}

void EmitConvertS16F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(int({}), 0, 16);", inst, value);
}

void EmitConvertS16F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(int({}), 0, 16);", inst, value);
}

void EmitConvertS32F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = int({});", inst, value);
}

void EmitConvertS32F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = int({});", inst, value);
}

void EmitConvertS32F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = int({});", inst, value);
}

void EmitConvertS64F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{} = int64_t({});", inst, value);
}

void EmitConvertS64F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{} = int64_t({});", inst, value);
}

void EmitConvertS64F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{} = int64_t({});", inst, value);
}

void EmitConvertU16F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(uint({}), 0, 16);", inst, value);
}

void EmitConvertU16F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(uint({}), 0, 16);", inst, value);
}

void EmitConvertU16F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(uint({}), 0, 16);", inst, value);
}

void EmitConvertU32F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = uint({});", inst, value);
}

void EmitConvertU16U32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(uint({}), 0, 16);", inst, value);
}

void EmitConvertU32U16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = uint(({} & 0xffffu));", inst, value);
}

void EmitConvertU8U32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = bitfieldExtract(uint({}), 0, 8);", inst, value);
}

void EmitConvertU32U8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = uint(({} & 0xffu));", inst, value);
}

void EmitConvertS32S8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = int(bitfieldExtract(int({}), 0, 8));", inst, value);
}

void EmitConvertS32S16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = int(bitfieldExtract(int({}), 0, 16));", inst, value);
}

void EmitConvertU32F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = uint({});", inst, value);
}

void EmitConvertU32F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = uint({});", inst, value);
}

void EmitConvertU64F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{} = uint64_t({});", inst, value);
}

void EmitConvertU64F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{} = uint64_t({});", inst, value);
}

void EmitConvertU64F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{} = uint64_t({});", inst, value);
}

void EmitConvertU64U32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU64("{} = uint64_t({});", inst, value);
}

void EmitConvertU32U64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU32("{} = uint({});", inst, value);
}

void EmitConvertF16F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float({}));", inst, value);
}

void EmitConvertF32F16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float({});", inst, value);
}

void EmitConvertF32F64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float({});", inst, value);
}

void EmitConvertF64F32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double({});", inst, value);
}

void EmitConvertF16S8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float(bitfieldExtract(int({}), 0, 8)));", inst, value);
}

void EmitConvertF16S16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float(bitfieldExtract(int({}), 0, 16)));", inst, value);
}

void EmitConvertF16S32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float(int({})));", inst, value);
}

void EmitConvertF16S64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float(int64_t({})));", inst, value);
}

void EmitConvertF16U8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float(({} & 0xffu)));", inst, value);
}

void EmitConvertF16U16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float(({} & 0xffffu)));", inst, value);
}

void EmitConvertF16U32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float({}));", inst, value);
}

void EmitConvertF16U64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = nx_half(float({}));", inst, value);
}

void EmitConvertF32S8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float(bitfieldExtract(int({}), 0, 8));", inst, value);
}

void EmitConvertF32S16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float(bitfieldExtract(int({}), 0, 16));", inst, value);
}

void EmitConvertF32S32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float(int({}));", inst, value);
}

void EmitConvertF32S64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float(int64_t({}));", inst, value);
}

void EmitConvertF32U8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float(({} & 0xffu));", inst, value);
}

void EmitConvertF32U16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float(({} & 0xffffu));", inst, value);
}

void EmitConvertF32U32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float({});", inst, value);
}

void EmitConvertF32U64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{} = float({});", inst, value);
}

void EmitConvertF64S8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double(bitfieldExtract(int({}), 0, 8));", inst, value);
}

void EmitConvertF64S16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double(bitfieldExtract(int({}), 0, 16));", inst, value);
}

void EmitConvertF64S32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double(int({}));", inst, value);
}

void EmitConvertF64S64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double(int64_t({}));", inst, value);
}

void EmitConvertF64U8(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double(({} & 0xffu));", inst, value);
}

void EmitConvertF64U16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double(({} & 0xffffu));", inst, value);
}

void EmitConvertF64U32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double({});", inst, value);
}

void EmitConvertF64U64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = double({});", inst, value);
}

} // namespace Shader::Backend::HLSL
