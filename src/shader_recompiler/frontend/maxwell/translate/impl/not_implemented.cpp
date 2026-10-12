// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {

[[noreturn]] static void ThrowNotImplemented(Opcode opcode) {
    throw NotImplementedException("Instruction {} is not implemented", opcode);
}

namespace {
// Byte permutation selectors follow PTX prmt; mode encodings follow envydis gm107.
// SASS orders the operands as source A, selector, source B (unlike PTX).
void PRMT(TranslatorVisitor& v, u64 insn, const IR::U32& source_b, const IR::U32& selector) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<48, 4, u64> mode;
    } const prmt{insn};
    if (prmt.mode > 6) {
        throw NotImplementedException("Invalid PRMT mode {}", prmt.mode.Value());
    }
    auto& ir = v.ir;
    const IR::U32 source_a{v.GetReg8(insn)};
    const IR::U32 control{ir.BitwiseAnd(selector, ir.Imm32(3))};
    IR::U32 result{ir.Imm32(0)};
    for (u32 byte = 0; byte < 4; ++byte) {
        IR::U32 index{ir.Imm32(0)};
        switch (prmt.mode) {
        case 0:
            index = ir.BitFieldExtract(selector, ir.Imm32(byte * 4), ir.Imm32(4), false);
            break;
        case 1: // Forward extraction.
            index = ir.IAdd(control, ir.Imm32(byte));
            break;
        case 2: // Backward extraction wraps around the eight source bytes.
            index = ir.BitwiseAnd(ir.ISub(control, ir.Imm32(byte)), ir.Imm32(7));
            break;
        case 3: // Replicate one byte.
            index = control;
            break;
        case 4: // Clamp to the left edge.
            index = ir.UMax(control, ir.Imm32(byte));
            break;
        case 5: // Clamp to the right edge.
            index = ir.UMin(control, ir.Imm32(byte));
            break;
        case 6: // Replicate one halfword; bit 1 of the control is ignored.
            index = ir.IAdd(ir.ShiftLeftLogical(ir.BitwiseAnd(control, ir.Imm32(1)), ir.Imm32(1)),
                            ir.Imm32(byte & 1));
            break;
        }
        const IR::U32 source{ir.Select(ir.IEqual(ir.BitwiseAnd(index, ir.Imm32(4)), ir.Imm32(0)),
                                       source_a, source_b)};
        const IR::U32 shift{ir.ShiftLeftLogical(ir.BitwiseAnd(index, ir.Imm32(3)), ir.Imm32(3))};
        IR::U32 value{ir.BitwiseAnd(ir.ShiftRightLogical(source, shift), ir.Imm32(255))};
        if (prmt.mode == 0) {
            const IR::U32 sign{
                ir.Select(ir.IEqual(ir.BitwiseAnd(value, ir.Imm32(128)), ir.Imm32(0)), ir.Imm32(0),
                          ir.Imm32(255))};
            value = IR::U32{
                ir.Select(ir.IEqual(ir.BitwiseAnd(index, ir.Imm32(8)), ir.Imm32(0)), value, sign)};
        }
        result = ir.BitwiseOr(result, ir.ShiftLeftLogical(value, ir.Imm32(byte * 8)));
    }
    v.X(prmt.dest_reg, result);
}

// Compute the high product using 16-bit limbs, without requiring native int64.
IR::U32 MultiplyHigh(IR::IREmitter& ir, const IR::U32& a, const IR::U32& b, bool signed_a,
                     bool signed_b) {
    const IR::U32 a_low{ir.BitwiseAnd(a, ir.Imm32(0xffff))};
    const IR::U32 b_low{ir.BitwiseAnd(b, ir.Imm32(0xffff))};
    const IR::U32 a_high{ir.ShiftRightLogical(a, ir.Imm32(16))};
    const IR::U32 b_high{ir.ShiftRightLogical(b, ir.Imm32(16))};
    const IR::U32 low{ir.IMul(a_low, b_low)};
    const IR::U32 middle{ir.IAdd(ir.IMul(a_high, b_low), ir.ShiftRightLogical(low, ir.Imm32(16)))};
    const IR::U32 carry{ir.ShiftRightLogical(middle, ir.Imm32(16))};
    const IR::U32 other{ir.IAdd(ir.IMul(a_low, b_high), ir.BitwiseAnd(middle, ir.Imm32(0xffff)))};
    IR::U32 high{ir.IAdd(ir.IAdd(ir.IMul(a_high, b_high), carry),
                         ir.ShiftRightLogical(other, ir.Imm32(16)))};
    if (signed_a) {
        high =
            ir.ISub(high, IR::U32{ir.Select(ir.ILessThan(a, ir.Imm32(0), true), b, ir.Imm32(0))});
    }
    if (signed_b) {
        high =
            ir.ISub(high, IR::U32{ir.Select(ir.ILessThan(b, ir.Imm32(0), true), a, ir.Imm32(0))});
    }
    return high;
}

void IMUL(TranslatorVisitor& v, u64 insn, const IR::U32& b, bool high, bool signed_a, bool signed_b,
          bool cc) {
    if (cc) {
        throw NotImplementedException("IMUL CC");
    }
    const IR::U32 a{v.GetReg8(insn)};
    const IR::U32 result{high ? MultiplyHigh(v.ir, a, b, signed_a, signed_b) : v.ir.IMul(a, b)};
    v.X(static_cast<IR::Reg>(insn & 255), result);
}

void IMUL(TranslatorVisitor& v, u64 insn, const IR::U32& b) {
    union {
        u64 raw;
        BitField<39, 1, u64> high;
        BitField<40, 1, u64> signed_a;
        BitField<41, 1, u64> signed_b;
        BitField<47, 1, u64> cc;
    } const imul{insn};
    IMUL(v, insn, b, imul.high != 0, imul.signed_a != 0, imul.signed_b != 0, imul.cc != 0);
}

void IMAD(TranslatorVisitor& v, u64 insn, const IR::U32& b, const IR::U32& c) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<47, 1, u64> cc;
        BitField<48, 1, u64> signed_a;
        BitField<49, 1, u64> x;
        BitField<50, 1, u64> sat;
        BitField<53, 1, u64> signed_b;
        BitField<54, 1, u64> high;
    } const imad{insn};
    if (imad.cc != 0) {
        throw NotImplementedException("IMAD CC");
    }
    if (imad.x != 0) {
        throw NotImplementedException("IMAD X");
    }
    if (imad.sat != 0) {
        throw NotImplementedException("IMAD SAT");
    }
    const IR::U32 a{v.GetReg8(insn)};
    const IR::U32 product{imad.high != 0
                              ? MultiplyHigh(v.ir, a, b, imad.signed_a != 0, imad.signed_b != 0)
                              : v.ir.IMul(a, b)};
    v.X(imad.dest_reg, v.ir.IAdd(product, c));
}
} // Anonymous namespace

void TranslatorVisitor::ATOM_cas(u64) {
    ThrowNotImplemented(Opcode::ATOM_cas);
}

void TranslatorVisitor::ATOMS_cas(u64) {
    ThrowNotImplemented(Opcode::ATOMS_cas);
}

void TranslatorVisitor::B2R(u64) {
    ThrowNotImplemented(Opcode::B2R);
}

void TranslatorVisitor::BPT(u64) {
    ThrowNotImplemented(Opcode::BPT);
}

void TranslatorVisitor::BRA(u64) {
    ThrowNotImplemented(Opcode::BRA);
}

void TranslatorVisitor::BRK(u64) {
    ThrowNotImplemented(Opcode::BRK);
}

void TranslatorVisitor::CAL(u64) {
    // CAL is a no-op
}

void TranslatorVisitor::CCTL(u64) {
    ThrowNotImplemented(Opcode::CCTL);
}

void TranslatorVisitor::CCTLL(u64) {
    ThrowNotImplemented(Opcode::CCTLL);
}

void TranslatorVisitor::CCTLT(u64) {
    ThrowNotImplemented(Opcode::CCTLT);
}

void TranslatorVisitor::CONT(u64) {
    ThrowNotImplemented(Opcode::CONT);
}

void TranslatorVisitor::CS2R(u64) {
    ThrowNotImplemented(Opcode::CS2R);
}

void TranslatorVisitor::FCHK_reg(u64) {
    ThrowNotImplemented(Opcode::FCHK_reg);
}

void TranslatorVisitor::FCHK_cbuf(u64) {
    ThrowNotImplemented(Opcode::FCHK_cbuf);
}

void TranslatorVisitor::FCHK_imm(u64) {
    ThrowNotImplemented(Opcode::FCHK_imm);
}

void TranslatorVisitor::GETCRSPTR(u64) {
    ThrowNotImplemented(Opcode::GETCRSPTR);
}

void TranslatorVisitor::GETLMEMBASE(u64) {
    ThrowNotImplemented(Opcode::GETLMEMBASE);
}

void TranslatorVisitor::IDE(u64) {
    ThrowNotImplemented(Opcode::IDE);
}

void TranslatorVisitor::IDP_reg(u64) {
    ThrowNotImplemented(Opcode::IDP_reg);
}

void TranslatorVisitor::IDP_imm(u64) {
    ThrowNotImplemented(Opcode::IDP_imm);
}

void TranslatorVisitor::IMAD_reg(u64 insn) {
    IMAD(*this, insn, GetReg20(insn), GetReg39(insn));
}

void TranslatorVisitor::IMAD_rc(u64 insn) {
    IMAD(*this, insn, GetReg39(insn), GetCbuf(insn));
}

void TranslatorVisitor::IMAD_cr(u64 insn) {
    IMAD(*this, insn, GetCbuf(insn), GetReg39(insn));
}

void TranslatorVisitor::IMAD_imm(u64 insn) {
    IMAD(*this, insn, GetImm20(insn), GetReg39(insn));
}

void TranslatorVisitor::IMAD32I(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<52, 1, u64> cc;
        BitField<53, 1, u64> high;
        BitField<54, 1, u64> signed_a;
        BitField<55, 1, u64> neg_c;
        BitField<56, 1, u64> neg_a;
        BitField<57, 1, u64> signed_b;
    } const imad{insn};
    if (imad.cc != 0) {
        throw NotImplementedException("IMAD32I CC");
    }
    if (imad.neg_a != 0 || imad.neg_c != 0) {
        throw NotImplementedException("IMAD32I NEG/PO");
    }
    const IR::U32 a{GetReg8(insn)};
    const IR::U32 b{GetImm32(insn)};
    const IR::U32 c{X(imad.dest_reg)};
    const IR::U32 product{imad.high != 0
                              ? MultiplyHigh(ir, a, b, imad.signed_a != 0, imad.signed_b != 0)
                              : ir.IMul(a, b)};
    X(imad.dest_reg, ir.IAdd(product, c));
}

void TranslatorVisitor::IMADSP_reg(u64) {
    ThrowNotImplemented(Opcode::IMADSP_reg);
}

void TranslatorVisitor::IMADSP_rc(u64) {
    ThrowNotImplemented(Opcode::IMADSP_rc);
}

void TranslatorVisitor::IMADSP_cr(u64) {
    ThrowNotImplemented(Opcode::IMADSP_cr);
}

void TranslatorVisitor::IMADSP_imm(u64) {
    ThrowNotImplemented(Opcode::IMADSP_imm);
}

void TranslatorVisitor::IMUL_reg(u64 insn) {
    IMUL(*this, insn, GetReg20(insn));
}

void TranslatorVisitor::IMUL_cbuf(u64 insn) {
    IMUL(*this, insn, GetCbuf(insn));
}

void TranslatorVisitor::IMUL_imm(u64 insn) {
    IMUL(*this, insn, GetImm20(insn));
}

void TranslatorVisitor::IMUL32I(u64 insn) {
    union {
        u64 raw;
        BitField<52, 1, u64> cc;
        BitField<53, 1, u64> high;
        BitField<54, 1, u64> signed_a;
        BitField<55, 1, u64> signed_b;
    } const imul{insn};
    IMUL(*this, insn, GetImm32(insn), imul.high != 0, imul.signed_a != 0, imul.signed_b != 0,
         imul.cc != 0);
}

void TranslatorVisitor::JCAL(u64) {
    ThrowNotImplemented(Opcode::JCAL);
}

void TranslatorVisitor::JMP(u64) {
    ThrowNotImplemented(Opcode::JMP);
}

void TranslatorVisitor::KIL(u64) {
    // KIL is a no-op
}

void TranslatorVisitor::LD(u64) {
    ThrowNotImplemented(Opcode::LD);
}

void TranslatorVisitor::LEPC(u64) {
    ThrowNotImplemented(Opcode::LEPC);
}

void TranslatorVisitor::LONGJMP(u64) {
    ThrowNotImplemented(Opcode::LONGJMP);
}

void TranslatorVisitor::NOP(u64) {
    // NOP is a no-op
}

void TranslatorVisitor::PBK(u64) {
    // PBK is a no-op
}

void TranslatorVisitor::PCNT(u64) {
    // PCNT is a no-op
}

void TranslatorVisitor::PEXIT(u64) {
    ThrowNotImplemented(Opcode::PEXIT);
}

void TranslatorVisitor::PLONGJMP(u64) {
    ThrowNotImplemented(Opcode::PLONGJMP);
}

void TranslatorVisitor::PRET(u64) {
    ThrowNotImplemented(Opcode::PRET);
}

void TranslatorVisitor::PRMT_reg(u64 insn) {
    PRMT(*this, insn, GetReg39(insn), GetReg20(insn));
}

void TranslatorVisitor::PRMT_rc(u64 insn) {
    PRMT(*this, insn, GetCbuf(insn), GetReg39(insn));
}

void TranslatorVisitor::PRMT_cr(u64 insn) {
    PRMT(*this, insn, GetReg39(insn), GetCbuf(insn));
}

void TranslatorVisitor::PRMT_imm(u64 insn) {
    PRMT(*this, insn, GetReg39(insn), GetImm20(insn));
}

void TranslatorVisitor::R2B(u64) {
    ThrowNotImplemented(Opcode::R2B);
}

void TranslatorVisitor::RAM(u64) {
    LOG_WARNING(Shader, "(STUBBED) RAM Instruction");
}

void TranslatorVisitor::RET(u64) {
    ThrowNotImplemented(Opcode::RET);
}

void TranslatorVisitor::RTT(u64) {
    ThrowNotImplemented(Opcode::RTT);
}

void TranslatorVisitor::SAM(u64) {
    LOG_WARNING(Shader, "(STUBBED) SAM Instruction");
}

void TranslatorVisitor::SETCRSPTR(u64) {
    ThrowNotImplemented(Opcode::SETCRSPTR);
}

void TranslatorVisitor::SETLMEMBASE(u64) {
    ThrowNotImplemented(Opcode::SETLMEMBASE);
}

void TranslatorVisitor::SSY(u64) {
    // SSY is a no-op
}

void TranslatorVisitor::ST(u64) {
    ThrowNotImplemented(Opcode::ST);
}

void TranslatorVisitor::STP(u64) {
    ThrowNotImplemented(Opcode::STP);
}

void TranslatorVisitor::SUATOM_cas(u64) {
    ThrowNotImplemented(Opcode::SUATOM_cas);
}

void TranslatorVisitor::SYNC(u64) {
    ThrowNotImplemented(Opcode::SYNC);
}

void TranslatorVisitor::TXA(u64) {
    ThrowNotImplemented(Opcode::TXA);
}

void TranslatorVisitor::VABSDIFF(u64) {
    ThrowNotImplemented(Opcode::VABSDIFF);
}

void TranslatorVisitor::VABSDIFF4(u64) {
    ThrowNotImplemented(Opcode::VABSDIFF4);
}

void TranslatorVisitor::VADD(u64) {
    ThrowNotImplemented(Opcode::VADD);
}

void TranslatorVisitor::VSET(u64) {
    ThrowNotImplemented(Opcode::VSET);
}
void TranslatorVisitor::VSHL(u64) {
    ThrowNotImplemented(Opcode::VSHL);
}

void TranslatorVisitor::VSHR(u64) {
    ThrowNotImplemented(Opcode::VSHR);
}

} // namespace Shader::Maxwell
