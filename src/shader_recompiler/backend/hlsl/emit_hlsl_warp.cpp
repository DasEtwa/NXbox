// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/runtime_info.h"
namespace Shader::Backend::HLSL {
void EmitLaneId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = WaveGetLaneIndex() & 31u;", inst);
}

void EmitVoteAll(EmitContext& ctx, IR::Inst& inst, std::string_view pred) {
    ctx.AddU1("{} = nx_ballot({}) == nx_ballot(true);", inst, pred);
}

void EmitVoteAny(EmitContext& ctx, IR::Inst& inst, std::string_view pred) {
    ctx.AddU1("{} = nx_ballot({}) != 0u;", inst, pred);
}

void EmitVoteEqual(EmitContext& ctx, IR::Inst& inst, std::string_view pred) {
    ctx.AddU1("{} = nx_ballot({}) == 0u || nx_ballot({}) == nx_ballot(true);", inst, pred, pred);
}

void EmitSubgroupBallot(EmitContext& ctx, IR::Inst& inst, std::string_view pred) {
    ctx.AddU32("{} = nx_ballot({});", inst, pred);
}

void EmitSubgroupEqMask(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = (1u << nx_lane());", inst);
}

void EmitSubgroupLtMask(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = ((1u << nx_lane()) - 1u);", inst);
}

void EmitSubgroupLeMask(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = (0xffffffffu >> (31u - nx_lane()));", inst);
}

void EmitSubgroupGtMask(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = ~(0xffffffffu >> (31u - nx_lane()));", inst);
}

void EmitSubgroupGeMask(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = ~((1u << nx_lane()) - 1u);", inst);
}

void EmitShuffleIndex(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                      std::string_view index, std::string_view clamp,
                      std::string_view segmentation_mask) {
    ctx.AddU32("{} = nx_shuffle({}, {}, {}, {}, 0u, shfl_in_bounds);", inst, value, index, clamp,
               segmentation_mask);
    if (auto* flag = inst.GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)) {
        ctx.AddU1("{} = shfl_in_bounds;", *flag);
        ctx.handled_pseudo.insert(flag);
    }
}

void EmitShuffleUp(EmitContext& ctx, IR::Inst& inst, std::string_view value, std::string_view index,
                   std::string_view clamp, std::string_view segmentation_mask) {
    ctx.AddU32("{} = nx_shuffle({}, {}, {}, {}, 1u, shfl_in_bounds);", inst, value, index, clamp,
               segmentation_mask);
    if (auto* flag = inst.GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)) {
        ctx.AddU1("{} = shfl_in_bounds;", *flag);
        ctx.handled_pseudo.insert(flag);
    }
}

void EmitShuffleDown(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                     std::string_view index, std::string_view clamp,
                     std::string_view segmentation_mask) {
    ctx.AddU32("{} = nx_shuffle({}, {}, {}, {}, 2u, shfl_in_bounds);", inst, value, index, clamp,
               segmentation_mask);
    if (auto* flag = inst.GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)) {
        ctx.AddU1("{} = shfl_in_bounds;", *flag);
        ctx.handled_pseudo.insert(flag);
    }
}

void EmitShuffleButterfly(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                          std::string_view index, std::string_view clamp,
                          std::string_view segmentation_mask) {
    ctx.AddU32("{} = nx_shuffle({}, {}, {}, {}, 3u, shfl_in_bounds);", inst, value, index, clamp,
               segmentation_mask);
    if (auto* flag = inst.GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)) {
        ctx.AddU1("{} = shfl_in_bounds;", *flag);
        ctx.handled_pseudo.insert(flag);
    }
}

void EmitQuadBroadcast(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                       std::string_view lane) {
    ctx.AddU32("{} = WaveReadLaneAt({}, (WaveGetLaneIndex() & ~3u) | ({} & 3u));", inst, value,
               lane);
}

void EmitQuadSwap(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                  std::string_view direction) {
    ctx.AddU32("{} = WaveReadLaneAt({}, WaveGetLaneIndex() ^ ({} + 1u));", inst, value, direction);
}

void EmitFSwizzleAdd(EmitContext& ctx, IR::Inst& inst, std::string_view op_a, std::string_view op_b,
                     std::string_view swizzle) {
    ctx.AddF32("{} = ({} * float4(-1, 1, -1, 0)[({} >> ((WaveGetLaneIndex() & 3u) * 2u)) & 3u]) + "
               "({} * float4(-1, -1, 1, -1)[({} >> ((WaveGetLaneIndex() & 3u) * 2u)) & 3u]);",
               inst, op_a, swizzle, op_b, swizzle);
}

void EmitDPdxFine(EmitContext& ctx, IR::Inst& inst, std::string_view op_a) {
    ctx.AddF32("{} = ddx_fine({});", inst, op_a);
}

void EmitDPdyFine(EmitContext& ctx, IR::Inst& inst, std::string_view op_a) {
    ctx.AddF32("{} = ddy_fine({});", inst, op_a);
}

void EmitDPdxCoarse(EmitContext& ctx, IR::Inst& inst, std::string_view op_a) {
    ctx.AddF32("{} = ddx_coarse({});", inst, op_a);
}

void EmitDPdyCoarse(EmitContext& ctx, IR::Inst& inst, std::string_view op_a) {
    ctx.AddF32("{} = ddy_coarse({});", inst, op_a);
}
} // namespace Shader::Backend::HLSL
