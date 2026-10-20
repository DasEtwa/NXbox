// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string_view>

#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Backend::HLSL {
namespace {
// Match the renderer's exact "1" bisect semantics without a dependency on D3D12.
bool BisectEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string_view{value} == "1";
}

void Compare(EmitContext& ctx, IR::Inst& inst, std::string_view lhs, std::string_view rhs,
             std::string_view op, bool ordered) {
    static const bool native_ordered = BisectEnabled("NXBOX_HLSL_NATIVE_ORDERED_COMPARE");
    // HLSL's relational/equality operators already reject NaNs. Ordered != still needs
    // explicit guards, and FP64 keeps its bit-based NaN helper. Opt in until Xbox DXC
    // output and rendered comparisons have been checked on the console.
    if (native_ordered && ordered && op != "!=" && inst.Arg(0).Type() != IR::Type::F64) {
        ctx.AddU1("{}={}{}{};", inst, lhs, op, rhs);
        return;
    }
    const auto nan_op{ordered ? "&&!" : "||"};
    ctx.AddU1("{}={}{}{}"
              "{}{}({}){}{}({});",
              inst, lhs, op, rhs, nan_op,
              inst.Arg(0).Type() == IR::Type::F64 ? "nx_isnan64" : "isnan", lhs, nan_op,
              inst.Arg(0).Type() == IR::Type::F64 ? "nx_isnan64" : "isnan", rhs);
}

bool IsPrecise(const IR::Inst& inst) {
    return inst.Flags<IR::FpControl>().no_contraction;
}
} // Anonymous namespace

void EmitFPAbs16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=nx_half(abs({}));", inst, value);
}

void EmitFPAbs32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=abs({});", inst, value);
}

void EmitFPAbs64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=nx_abs64({});", inst, value);
}

void EmitFPAdd16(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF32("{}=nx_half({}+{});", inst, a, b);
    } else {
        ctx.AddF32("{}=nx_half({}+{});", inst, a, b);
    }
}

void EmitFPAdd32(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF32("{}={}+{};", inst, a, b);
    } else {
        ctx.AddF32("{}={}+{};", inst, a, b);
    }
}

void EmitFPAdd64(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF64("{}={}+{};", inst, a, b);
    } else {
        ctx.AddF64("{}={}+{};", inst, a, b);
    }
}

void EmitFPFma16(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b,
                 std::string_view c) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF32("{}=nx_half(mad({},{},{}));", inst, a, b, c);
    } else {
        ctx.AddF32("{}=nx_half(mad({},{},{}));", inst, a, b, c);
    }
}

void EmitFPFma32(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b,
                 std::string_view c) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF32("{}=mad({},{},{});", inst, a, b, c);
    } else {
        ctx.AddF32("{}=mad({},{},{});", inst, a, b, c);
    }
}

void EmitFPFma64(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b,
                 std::string_view c) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF64("{}=fma({},{},{});", inst, a, b, c);
    } else {
        ctx.AddF64("{}=fma({},{},{});", inst, a, b, c);
    }
}

void EmitFPMax32(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    ctx.AddF32("{}=max({},{});", inst, a, b);
}

void EmitFPMax64(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    ctx.AddF64("{}=nx_max64({},{});", inst, a, b);
}

void EmitFPMin32(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    ctx.AddF32("{}=min({},{});", inst, a, b);
}

void EmitFPMin64(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    ctx.AddF64("{}=nx_min64({},{});", inst, a, b);
}

void EmitFPMul16(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF32("{}=nx_half({}*{});", inst, a, b);
    } else {
        ctx.AddF32("{}=nx_half({}*{});", inst, a, b);
    }
}

void EmitFPMul32(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF32("{}={}*{};", inst, a, b);
    } else {
        ctx.AddF32("{}={}*{};", inst, a, b);
    }
}

void EmitFPMul64(EmitContext& ctx, IR::Inst& inst, std::string_view a, std::string_view b) {
    if (IsPrecise(inst)) {
        ctx.AddPrecF64("{}={}*{};", inst, a, b);
    } else {
        ctx.AddF64("{}={}*{};", inst, a, b);
    }
}

void EmitFPNeg16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=nx_half(0.f-({}));", inst, value);
}

void EmitFPNeg32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=0.f-({});", inst, value);
}

void EmitFPNeg64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=double(0.)-({});", inst, value);
}

void EmitFPSin(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=sin({});", inst, value);
}

void EmitFPCos(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=cos({});", inst, value);
}

void EmitFPExp2(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=exp2({});", inst, value);
}

void EmitFPLog2(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=log2({});", inst, value);
}

void EmitFPRecip32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=(1.0f)/{};", inst, value);
}

void EmitFPRecip64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=1.0/{};", inst, value);
}

void EmitFPRecipSqrt32([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                       [[maybe_unused]] std::string_view value) {
    ctx.AddF32("{}=rsqrt({});", inst, value);
}

void EmitFPRecipSqrt64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{} = nx_rsqrt64({});", inst, value);
}

void EmitFPSqrt(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=sqrt({});", inst, value);
}

void EmitFPSaturate16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=nx_half(min(max({},0.0),1.0));", inst, value);
}

void EmitFPSaturate32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=min(max({},0.0),1.0);", inst, value);
}

void EmitFPSaturate64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=nx_min64(nx_max64({},0.0),1.0);", inst, value);
}

void EmitFPClamp16(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                   std::string_view min_value, std::string_view max_value) {
    // HLSL's clamp does not produce desirable results
    ctx.AddF32("{}=nx_half(min(max({},float({})),float({})));", inst, value, min_value, max_value);
}

void EmitFPClamp32(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                   std::string_view min_value, std::string_view max_value) {
    // HLSL's clamp does not produce desirable results
    ctx.AddF32("{}=min(max({},float({})),float({}));", inst, value, min_value, max_value);
}

void EmitFPClamp64(EmitContext& ctx, IR::Inst& inst, std::string_view value,
                   std::string_view min_value, std::string_view max_value) {
    // HLSL's clamp does not produce desirable results
    ctx.AddF64("{}=nx_min64(nx_max64({},double({})),double({}));", inst, value, min_value,
               max_value);
}

void EmitFPRoundEven16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=nx_half(round({}));", inst, value);
}

void EmitFPRoundEven32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=round({});", inst, value);
}

void EmitFPRoundEven64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=nx_round64({});", inst, value);
}

void EmitFPFloor16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=nx_half(floor({}));", inst, value);
}

void EmitFPFloor32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=floor({});", inst, value);
}

void EmitFPFloor64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=nx_floor64({});", inst, value);
}

void EmitFPCeil16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=nx_half(ceil({}));", inst, value);
}

void EmitFPCeil32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=ceil({});", inst, value);
}

void EmitFPCeil64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=nx_ceil64({});", inst, value);
}

void EmitFPTrunc16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=nx_half(trunc({}));", inst, value);
}

void EmitFPTrunc32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF32("{}=trunc({});", inst, value);
}

void EmitFPTrunc64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddF64("{}=nx_trunc64({});", inst, value);
}

void EmitFPOrdEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                      std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "==", true);
}

void EmitFPOrdEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                      std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "==", true);
}

void EmitFPOrdEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                      std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "==", true);
}

void EmitFPUnordEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                        std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "==", false);
}

void EmitFPUnordEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                        std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "==", false);
}

void EmitFPUnordEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                        std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "==", false);
}

void EmitFPOrdNotEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                         std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "!=", true);
}

void EmitFPOrdNotEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                         std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "!=", true);
}

void EmitFPOrdNotEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                         std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "!=", true);
}

void EmitFPUnordNotEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                           std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "!=", false);
}

void EmitFPUnordNotEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                           std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "!=", false);
}

void EmitFPUnordNotEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                           std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "!=", false);
}

void EmitFPOrdLessThan16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                         std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<", true);
}

void EmitFPOrdLessThan32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                         std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<", true);
}

void EmitFPOrdLessThan64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                         std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<", true);
}

void EmitFPUnordLessThan16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                           std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<", false);
}

void EmitFPUnordLessThan32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                           std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<", false);
}

void EmitFPUnordLessThan64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                           std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<", false);
}

void EmitFPOrdGreaterThan16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                            std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">", true);
}

void EmitFPOrdGreaterThan32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                            std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">", true);
}

void EmitFPOrdGreaterThan64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                            std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">", true);
}

void EmitFPUnordGreaterThan16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                              std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">", false);
}

void EmitFPUnordGreaterThan32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                              std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">", false);
}

void EmitFPUnordGreaterThan64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                              std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">", false);
}

void EmitFPOrdLessThanEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                              std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<=", true);
}

void EmitFPOrdLessThanEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                              std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<=", true);
}

void EmitFPOrdLessThanEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                              std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<=", true);
}

void EmitFPUnordLessThanEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<=", false);
}

void EmitFPUnordLessThanEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<=", false);
}

void EmitFPUnordLessThanEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, "<=", false);
}

void EmitFPOrdGreaterThanEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                 std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">=", true);
}

void EmitFPOrdGreaterThanEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                 std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">=", true);
}

void EmitFPOrdGreaterThanEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                 std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">=", true);
}

void EmitFPUnordGreaterThanEqual16(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                   std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">=", false);
}

void EmitFPUnordGreaterThanEqual32(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                   std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">=", false);
}

void EmitFPUnordGreaterThanEqual64(EmitContext& ctx, IR::Inst& inst, std::string_view lhs,
                                   std::string_view rhs) {
    Compare(ctx, inst, lhs, rhs, ">=", false);
}

void EmitFPIsNan16(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU1("{}=isnan({});", inst, value);
}

void EmitFPIsNan32(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU1("{}=isnan({});", inst, value);
}

void EmitFPIsNan64(EmitContext& ctx, IR::Inst& inst, std::string_view value) {
    ctx.AddU1("{}=nx_isnan64({});", inst, value);
}

} // namespace Shader::Backend::HLSL
