// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Backend::HLSL {
namespace {
constexpr std::string_view SWIZZLE{"xyzw"};
void CompositeInsert(EmitContext& ctx, std::string_view result, std::string_view composite,
                     std::string_view object, u32 index) {
    if (result == composite) {
        // The result is aliased with the composite
        ctx.Add("{}.{}={};", composite, SWIZZLE[index], object);
    } else {
        ctx.Add("{}={};{}.{}={};", result, composite, result, SWIZZLE[index], object);
    }
}
} // Anonymous namespace

void EmitCompositeConstructU32x2(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2) {
    ctx.AddU32x2("{}=uint2({},{});", inst, e1, e2);
}

void EmitCompositeConstructU32x3(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3) {
    ctx.AddU32x3("{}=uint3({},{},{});", inst, e1, e2, e3);
}

void EmitCompositeConstructU32x4(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3, std::string_view e4) {
    ctx.AddU32x4("{}=uint4({},{},{},{});", inst, e1, e2, e3, e4);
}

void EmitCompositeExtractU32x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddU32("{}={}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractU32x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddU32("{}={}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractU32x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddU32("{}={}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeInsertU32x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto ret{ctx.var_alloc.Define(inst, HlslVarType::U32x2)};
    CompositeInsert(ctx, ret, composite, object, index);
}

void EmitCompositeInsertU32x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto ret{ctx.var_alloc.Define(inst, HlslVarType::U32x3)};
    CompositeInsert(ctx, ret, composite, object, index);
}

void EmitCompositeInsertU32x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto ret{ctx.var_alloc.Define(inst, HlslVarType::U32x4)};
    CompositeInsert(ctx, ret, composite, object, index);
}

void EmitCompositeConstructF16x2(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F16x2);
    ctx.Add("{} = float2({},{});", result, e1, e2);
}

void EmitCompositeConstructF16x3(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F16x3);
    ctx.Add("{} = float3({},{},{});", result, e1, e2, e3);
}

void EmitCompositeConstructF16x4(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3, std::string_view e4) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F16x4);
    ctx.Add("{} = float4({},{},{},{});", result, e1, e2, e3, e4);
}

void EmitCompositeExtractF16x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF32("{} = {}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractF16x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF32("{} = {}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractF16x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF32("{} = {}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeInsertF16x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F16x2);
    CompositeInsert(ctx, result, composite, object, index);
}

void EmitCompositeInsertF16x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F16x3);
    CompositeInsert(ctx, result, composite, object, index);
}

void EmitCompositeInsertF16x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F16x4);
    CompositeInsert(ctx, result, composite, object, index);
}

void EmitCompositeConstructF32x2(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2) {
    ctx.AddF32x2("{}=float2({},{});", inst, e1, e2);
}

void EmitCompositeConstructF32x3(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3) {
    ctx.AddF32x3("{}=float3({},{},{});", inst, e1, e2, e3);
}

void EmitCompositeConstructF32x4(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3, std::string_view e4) {
    ctx.AddF32x4("{}=float4({},{},{},{});", inst, e1, e2, e3, e4);
}

void EmitCompositeExtractF32x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF32("{}={}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractF32x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF32("{}={}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractF32x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF32("{}={}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeInsertF32x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto ret{ctx.var_alloc.Define(inst, HlslVarType::F32x2)};
    CompositeInsert(ctx, ret, composite, object, index);
}

void EmitCompositeInsertF32x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto ret{ctx.var_alloc.Define(inst, HlslVarType::F32x3)};
    CompositeInsert(ctx, ret, composite, object, index);
}

void EmitCompositeInsertF32x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto ret{ctx.var_alloc.Define(inst, HlslVarType::F32x4)};
    CompositeInsert(ctx, ret, composite, object, index);
}

void EmitCompositeConstructF64x2(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F64x2);
    ctx.Add("{} = double2({},{});", result, e1, e2);
}

void EmitCompositeConstructF64x3(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F64x3);
    ctx.Add("{} = double3({},{},{});", result, e1, e2, e3);
}

void EmitCompositeConstructF64x4(EmitContext& ctx, IR::Inst& inst, std::string_view e1,
                                 std::string_view e2, std::string_view e3, std::string_view e4) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F64x4);
    ctx.Add("{} = double4({},{},{},{});", result, e1, e2, e3, e4);
}

void EmitCompositeExtractF64x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF64("{} = {}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractF64x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF64("{} = {}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeExtractF64x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                               u32 index) {
    ctx.AddF64("{} = {}.{};", inst, composite, SWIZZLE[index]);
}

void EmitCompositeInsertF64x2(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F64x2);
    CompositeInsert(ctx, result, composite, object, index);
}

void EmitCompositeInsertF64x3(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F64x3);
    CompositeInsert(ctx, result, composite, object, index);
}

void EmitCompositeInsertF64x4(EmitContext& ctx, IR::Inst& inst, std::string_view composite,
                              std::string_view object, u32 index) {
    const auto result = ctx.var_alloc.Define(inst, IR::Type::F64x4);
    CompositeInsert(ctx, result, composite, object, index);
}
} // namespace Shader::Backend::HLSL
