// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
namespace Shader::Backend::HLSL {
void EmitPhi(EmitContext& ctx, IR::Inst& phi) {
    ctx.var_alloc.PhiDefine(phi, phi.Type());
}
void EmitVoid(EmitContext&) {}
void EmitReference(EmitContext&, const IR::Value&) {}
void EmitPhiMove(EmitContext& ctx, const IR::Value& phi, const IR::Value& value) {
    ctx.Add("{} = {};", ctx.var_alloc.Consume(phi), ctx.var_alloc.Consume(value));
}
void EmitPrologue(EmitContext&) {}
void EmitEpilogue(EmitContext&) {}
void EmitJoin(EmitContext&) {
    throw NotImplementedException("HLSL Join must be removed by structured control-flow lowering");
}
void EmitDemoteToHelperInvocation(EmitContext& ctx) {
    if (ctx.stage != Stage::Fragment)
        throw NotImplementedException("HLSL discard outside fragment stage");
    ctx.Add("discard;");
}
void EmitBarrier(EmitContext& ctx) {
    ctx.Add("GroupMemoryBarrierWithGroupSync();");
}
void EmitWorkgroupMemoryBarrier(EmitContext& ctx) {
    ctx.Add("GroupMemoryBarrier();");
}
void EmitDeviceMemoryBarrier(EmitContext& ctx) {
    ctx.Add("DeviceMemoryBarrier();");
}
} // namespace Shader::Backend::HLSL
