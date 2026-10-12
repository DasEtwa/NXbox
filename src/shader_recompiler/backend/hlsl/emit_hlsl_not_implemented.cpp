// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
namespace Shader::Backend::HLSL {
void EmitGetRegister(EmitContext& ctx, IR::Inst& inst, IR::Reg reg) {
    ctx.uses_register_state = true;
    ctx.AddU32("{} = {};", inst,
               static_cast<u32>(reg) == 255u ? "0u"
                                             : fmt::format("nx_regs[{}]", static_cast<u32>(reg)));
}
void EmitSetRegister(EmitContext& ctx, IR::Reg reg, std::string_view value) {
    ctx.uses_register_state = true;
    if (static_cast<u32>(reg) != 255u)
        ctx.Add("nx_regs[{}] = {};", static_cast<u32>(reg), value);
}
void EmitGetPred(EmitContext& ctx, IR::Inst& inst, IR::Pred pred) {
    ctx.uses_register_state = true;
    ctx.AddU1("{} = {};", inst,
              static_cast<u32>(pred) == 7u ? "true"
                                           : fmt::format("nx_preds[{}]", static_cast<u32>(pred)));
}
void EmitSetPred(EmitContext& ctx, IR::Pred pred, std::string_view value) {
    ctx.uses_register_state = true;
    if (static_cast<u32>(pred) != 7u)
        ctx.Add("nx_preds[{}] = {};", static_cast<u32>(pred), value);
}
void EmitGetGotoVariable(EmitContext& ctx, IR::Inst& inst, u32 index) {
    ctx.goto_variables.insert(index);
    ctx.AddU1("{} = nx_goto_{};", inst, index);
}
void EmitSetGotoVariable(EmitContext& ctx, u32 index, std::string_view value) {
    ctx.goto_variables.insert(index);
    ctx.Add("nx_goto_{} = {};", index, value);
}
void EmitGetIndirectBranchVariable(EmitContext& ctx, IR::Inst& inst) {
    ctx.uses_register_state = true;
    ctx.AddU32("{} = nx_branch;", inst);
}
void EmitSetIndirectBranchVariable(EmitContext& ctx, std::string_view value) {
    ctx.uses_register_state = true;
    ctx.Add("nx_branch = {};", value);
}
void EmitGetZFlag(EmitContext& ctx, IR::Inst& inst) {
    ctx.uses_register_state = true;
    ctx.AddU1("{} = nx_z;", inst);
}
void EmitSetZFlag(EmitContext& ctx, std::string_view value) {
    ctx.uses_register_state = true;
    ctx.Add("nx_z = {};", value);
}
void EmitGetSFlag(EmitContext& ctx, IR::Inst& inst) {
    ctx.uses_register_state = true;
    ctx.AddU1("{} = nx_s;", inst);
}
void EmitSetSFlag(EmitContext& ctx, std::string_view value) {
    ctx.uses_register_state = true;
    ctx.Add("nx_s = {};", value);
}
void EmitGetCFlag(EmitContext& ctx, IR::Inst& inst) {
    ctx.uses_register_state = true;
    ctx.AddU1("{} = nx_c;", inst);
}
void EmitSetCFlag(EmitContext& ctx, std::string_view value) {
    ctx.uses_register_state = true;
    ctx.Add("nx_c = {};", value);
}
void EmitGetOFlag(EmitContext& ctx, IR::Inst& inst) {
    ctx.uses_register_state = true;
    ctx.AddU1("{} = nx_o;", inst);
}
void EmitSetOFlag(EmitContext& ctx, std::string_view value) {
    ctx.uses_register_state = true;
    ctx.Add("nx_o = {};", value);
}
void EmitGetZeroFromOp(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU1("{} = {} == 0u;", inst, value);
}
void EmitGetSignFromOp(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU1("{} = int({}) < 0;", inst, value);
}
void EmitGetCarryFromOp(EmitContext& ctx, IR::Inst& inst, const IR::Value& value) {
    const auto* producer = value.InstRecursive();
    if (producer->GetOpcode() != IR::Opcode::IAdd32)
        throw NotImplementedException("HLSL carry pseudo-result producer {}",
                                      producer->GetOpcode());
    ctx.AddU1("{} = {} < {};", inst, ctx.var_alloc.Consume(value),
              ctx.var_alloc.Consume(producer->Arg(0)));
}
void EmitGetOverflowFromOp(EmitContext& ctx, IR::Inst& inst, const IR::Value& value) {
    const auto* producer = value.InstRecursive();
    if (producer->GetOpcode() != IR::Opcode::IAdd32)
        throw NotImplementedException("HLSL overflow pseudo-result producer {}",
                                      producer->GetOpcode());
    ctx.AddU1("{} = ((~({} ^ {}) & ({} ^ {})) & 0x80000000u) != 0u;", inst,
              ctx.var_alloc.Consume(producer->Arg(0)), ctx.var_alloc.Consume(producer->Arg(1)),
              ctx.var_alloc.Consume(producer->Arg(0)), ctx.var_alloc.Consume(value));
}
void EmitGetSparseFromOp(EmitContext& ctx, IR::Inst& inst, const IR::Value&) {
    // Native D3D12 resources are fully committed; true means resident.
    ctx.AddU1("{} = true;", inst);
}
void EmitGetInBoundsFromOp(EmitContext& ctx, IR::Inst& inst, const IR::Value&) {
    ctx.AddU1("{} = shfl_in_bounds;", inst);
}
} // namespace Shader::Backend::HLSL
