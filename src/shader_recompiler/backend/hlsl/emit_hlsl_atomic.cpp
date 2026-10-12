// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/runtime_info.h"
namespace Shader::Backend::HLSL {
void EmitSharedAtomicIAdd32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedAdd(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicSMin32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = "
            "uint(min(int(old_value), int({}))); InterlockedCompareExchange(smem[{} >> 2], "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            o, value, o, result);
}

void EmitSharedAtomicUMin32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedMin(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicSMax32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = "
            "uint(max(int(old_value), int({}))); InterlockedCompareExchange(smem[{} >> 2], "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            o, value, o, result);
}

void EmitSharedAtomicUMax32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedMax(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicInc32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = old_value >= "
            "{} ? 0u : old_value + 1u; InterlockedCompareExchange(smem[{} >> 2], old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            o, value, o, result);
}

void EmitSharedAtomicDec32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = old_value == "
            "0u || old_value > {} ? {} : old_value - 1u; InterlockedCompareExchange(smem[{} >> 2], "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            o, value, value, o, result);
}

void EmitSharedAtomicAnd32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedAnd(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicOr32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                          std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedOr(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicXor32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedXor(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicExchange32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                                std::string_view value) {
    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedExchange(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitStorageAtomicIAdd32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedAdd({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicSMin32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = "
            "uint(min(int(old_value), int({}))); {}.InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            resource, o, value, resource, o, result);
}

void EmitStorageAtomicUMin32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedMin({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicSMax32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = "
            "uint(max(int(old_value), int({}))); {}.InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            resource, o, value, resource, o, result);
}

void EmitStorageAtomicUMax32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedMax({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicInc32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = old_value >= "
            "{} ? 0u : old_value + 1u; {}.InterlockedCompareExchange({}, old_value, new_value, "
            "previous); if (previous == old_value) break; old_value = previous; }} while (true); "
            "{} = old_value; }}",
            resource, o, value, resource, o, result);
}

void EmitStorageAtomicDec32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = old_value == "
            "0u || old_value > {} ? {} : old_value - 1u; {}.InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            resource, o, value, value, resource, o, result);
}

void EmitStorageAtomicAnd32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedAnd({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicOr32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                           const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedOr({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicXor32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedXor({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicExchange32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                                 const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo{}", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedExchange({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicAddF32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::F32);
    ctx.Add("{{ uint old_value = ssbo{}.Load({}); uint previous; do {{ uint new_value = "
            "asuint(asfloat(old_value) + {}); ssbo{}.InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = asfloat(old_value); }}",
            binding.U32(), o, value, binding.U32(), o, result);
}
void EmitStorageAtomicAddF16x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo{}.Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(unpackHalf2x16(old_value) + {}); ssbo{}.InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMinF16x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo{}.Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(min(unpackHalf2x16(old_value), {})); "
            "ssbo{}.InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMaxF16x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo{}.Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(max(unpackHalf2x16(old_value), {})); "
            "ssbo{}.InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicAddF32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo{}.Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(unpackHalf2x16(old_value) + {}); ssbo{}.InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMinF32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo{}.Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(min(unpackHalf2x16(old_value), {})); "
            "ssbo{}.InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMaxF32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo{}.Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(max(unpackHalf2x16(old_value), {})); "
            "ssbo{}.InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicIAdd32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo{}.InterlockedAdd({}, {}.x, {}.x); ssbo{}.InterlockedAdd({} + 4u, {}.y, {}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicSMin32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("for (uint component = 0u; component < 2u; ++component) {{ uint byte_offset = {} + "
            "component * 4u; uint old_value = ssbo{}.Load(byte_offset); uint previous; do {{ uint "
            "new_value = uint(min(int(old_value), int({}[component]))); "
            "ssbo{}.InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if "
            "(previous == old_value) break; old_value = previous; }} while (true); {}[component] = "
            "old_value; }}",
            o, binding.U32(), value, binding.U32(), result);
}

void EmitStorageAtomicUMin32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo{}.InterlockedMin({}, {}.x, {}.x); ssbo{}.InterlockedMin({} + 4u, {}.y, {}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicSMax32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("for (uint component = 0u; component < 2u; ++component) {{ uint byte_offset = {} + "
            "component * 4u; uint old_value = ssbo{}.Load(byte_offset); uint previous; do {{ uint "
            "new_value = uint(max(int(old_value), int({}[component]))); "
            "ssbo{}.InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if "
            "(previous == old_value) break; old_value = previous; }} while (true); {}[component] = "
            "old_value; }}",
            o, binding.U32(), value, binding.U32(), result);
}

void EmitStorageAtomicUMax32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo{}.InterlockedMax({}, {}.x, {}.x); ssbo{}.InterlockedMax({} + 4u, {}.y, {}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicAnd32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                              const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo{}.InterlockedAnd({}, {}.x, {}.x); ssbo{}.InterlockedAnd({} + 4u, {}.y, {}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicOr32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo{}.InterlockedOr({}, {}.x, {}.x); ssbo{}.InterlockedOr({} + 4u, {}.y, {}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicXor32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                              const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo{}.InterlockedXor({}, {}.x, {}.x); ssbo{}.InterlockedXor({} + 4u, {}.y, {}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicExchange32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                                   const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo{}.InterlockedExchange({}, {}.x, {}.x); ssbo{}.InterlockedExchange({} + 4u, {}.y, "
            "{}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}
} // namespace Shader::Backend::HLSL
