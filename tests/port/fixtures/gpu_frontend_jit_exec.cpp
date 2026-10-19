// SPDX-License-Identifier: GPL-3.0-or-later
// Executes the *production* MacroJITx64Impl::Compile_ALU text against recording
// stubs and a tiny x86 model, comparing with the production interpreter's
// GetALUResult.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string_view>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#define UNIMPLEMENTED_MSG(...)                                                                     \
    do {                                                                                           \
        ++unimplemented_hits;                                                                      \
    } while (false)
static unsigned unimplemented_hits;
namespace Core {
struct System {};
} // namespace Core
namespace Macro {
enum class ALUOperation : u32 {
    Add = 0,
    AddWithCarry = 1,
    Subtract = 2,
    SubtractWithBorrow = 3,
    Xor = 8,
    Or = 9,
    And = 10,
    AndNot = 11,
    Nand = 12
};
enum class ResultOperation : u32 { Move = 1 };
struct AluField {
    ALUOperation v;
    operator ALUOperation() const {
        return v;
    }
    u32 Value() const {
        return static_cast<u32>(v);
    }
};
struct Opcode {
    u32 src_a, src_b, dst;
    AluField alu_operation;
    ResultOperation result_operation;
};
} // namespace Macro
namespace Xbyak {
struct Reg32 {
    int idx{-1};
    Reg32() = default;
    constexpr Reg32(int i) : idx(i) {}
};
struct Reg64 {
    int idx;
};
} // namespace Xbyak
struct Addr {
    long off;
};
template <class T>
Addr operator+(Xbyak::Reg64, T o) {
    return {static_cast<long>(o)};
}
template <class T>
Addr operator+(Addr a, T o) {
    return {a.off + static_cast<long>(o)};
}
struct Mem {
    long off;
    int size;
};
struct MemSpace {
    int size;
    Mem operator[](Addr a) const {
        return {a.off, size};
    }
};
constexpr Xbyak::Reg64 STATE{3};
constexpr Xbyak::Reg32 RESULT{10};
struct Insn {
    enum K { Add, Sub, Adc, Sbb, Xor, Or, And, Not, Load, Bt, Setc } k;
    int d, s;
    long off;
};
struct MacroJITx64Impl {
    struct JITState {
        void* maxwell3d;
        void* system;
        std::array<u32, 8> registers;
        u32 carry_flag;
    };
    struct OptimizerState {
        bool can_skip_carry{}, has_delayed_pc{}, zero_reg_skip{}, skip_dummy_addimmediate{},
            optimize_for_method_move{}, enable_asserts{};
    } optimizer;
    std::vector<Insn> code;
    MemSpace dword{4}, byte{1};
    const Xbyak::Reg32 eax{0};
    void add(Xbyak::Reg32 d, Xbyak::Reg32 s) {
        code.push_back({Insn::Add, d.idx, s.idx, 0});
    }
    void sub(Xbyak::Reg32 d, Xbyak::Reg32 s) {
        code.push_back({Insn::Sub, d.idx, s.idx, 0});
    }
    void adc(Xbyak::Reg32 d, Xbyak::Reg32 s) {
        code.push_back({Insn::Adc, d.idx, s.idx, 0});
    }
    void sbb(Xbyak::Reg32 d, Xbyak::Reg32 s) {
        code.push_back({Insn::Sbb, d.idx, s.idx, 0});
    }
    void xor_(Xbyak::Reg32 d, Xbyak::Reg32 s) {
        code.push_back({Insn::Xor, d.idx, s.idx, 0});
    }
    void or_(Xbyak::Reg32 d, Xbyak::Reg32 s) {
        code.push_back({Insn::Or, d.idx, s.idx, 0});
    }
    void and_(Xbyak::Reg32 d, Xbyak::Reg32 s) {
        code.push_back({Insn::And, d.idx, s.idx, 0});
    }
    void not_(Xbyak::Reg32 d) {
        code.push_back({Insn::Not, d.idx, 0, 0});
    }
    void mov(Xbyak::Reg32 d, Mem m) {
        assert(m.size == 4);
        code.push_back({Insn::Load, d.idx, 0, m.off});
    }
    void bt(Mem m, int bit) {
        assert(m.size == 4 && bit == 0);
        code.push_back({Insn::Bt, 0, 0, m.off});
    }
    void setc(Mem m) {
        assert(m.size == 1);
        code.push_back({Insn::Setc, 0, 0, m.off});
    }
    void Compile_ProcessResult(Core::System&, Macro::ResultOperation, u32) {}
    void Compile_ALU(Core::System& system, Macro::Opcode opcode);
    Xbyak::Reg32 Compile_GetRegister(u32 index, Xbyak::Reg32 dst);
};
struct MacroInterpreterImpl {
    bool carry_flag{};
    u32 GetALUResult(Macro::ALUOperation operation, u32 src_a, u32 src_b);
};
// PRODUCTION
int main(int argc, char** argv) {
    const bool expect_failures = argc > 1 && std::string_view(argv[1]) == "--expect-failures";
    std::mt19937 rng{12345};
    unsigned cases = 0, mismatches = 0, first_reports = 0;
    const Macro::ALUOperation ops[] = {
        Macro::ALUOperation::Add,      Macro::ALUOperation::AddWithCarry,
        Macro::ALUOperation::Subtract, Macro::ALUOperation::SubtractWithBorrow,
        Macro::ALUOperation::Xor,      Macro::ALUOperation::Or,
        Macro::ALUOperation::And,      Macro::ALUOperation::AndNot,
        Macro::ALUOperation::Nand};
    const u32 interesting[] = {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu};
    for (auto op : ops)
        for (u32 a = 0; a < 8; ++a)
            for (u32 b = 0; b < 8; ++b)
                for (int can_skip = 0; can_skip < 2; ++can_skip)
                    for (int trial = 0; trial < 40; ++trial) {
                        MacroJITx64Impl jit;
                        jit.optimizer.zero_reg_skip = true;
                        jit.optimizer.can_skip_carry = can_skip;
                        Core::System sys;
                        Macro::Opcode opcode{a, b, 1, {op}, Macro::ResultOperation::Move};
                        jit.Compile_ALU(sys, opcode);
                        MacroJITx64Impl::JITState state{};
                        for (u32 r = 0; r < 8; ++r)
                            state.registers[r] = (rng() & 1) ? interesting[rng() % 6] : rng();
                        state.registers[0] = rng(); // slot 0 must never be read: register 0
                                                    // is hardwired to zero
                        state.carry_flag = rng() & 1;
                        const u32 carry_before = state.carry_flag;
                        alignas(8) unsigned char mem[sizeof(state)];
                        std::memcpy(mem, &state, sizeof(state));
                        u32 r[16];
                        for (auto& x : r)
                            x = rng();
                        bool cf = rng() & 1;
                        for (const auto& i : jit.code) {
                            u32& d = r[i.d];
                            const u32 s = r[i.s];
                            switch (i.k) {
                            case Insn::Add: {
                                u64 t = u64(d) + s;
                                cf = t >> 32;
                                d = u32(t);
                                break;
                            }
                            case Insn::Adc: {
                                u64 t = u64(d) + s + cf;
                                cf = t >> 32;
                                d = u32(t);
                                break;
                            }
                            case Insn::Sub: {
                                cf = d < s;
                                d = d - s;
                                break;
                            }
                            case Insn::Sbb: {
                                bool nb = u64(d) < u64(s) + cf;
                                d = d - s - cf;
                                cf = nb;
                                break;
                            }
                            case Insn::Xor:
                                d ^= s;
                                cf = false;
                                break;
                            case Insn::Or:
                                d |= s;
                                cf = false;
                                break;
                            case Insn::And:
                                d &= s;
                                cf = false;
                                break;
                            case Insn::Not:
                                d = ~d;
                                break;
                            case Insn::Load:
                                std::memcpy(&d, mem + i.off, 4);
                                break;
                            case Insn::Bt: {
                                u32 v;
                                std::memcpy(&v, mem + i.off, 4);
                                cf = v & 1;
                                break;
                            }
                            case Insn::Setc:
                                mem[i.off] = cf;
                                break;
                            }
                        }
                        u32 carry_after;
                        std::memcpy(&carry_after,
                                    mem + offsetof(MacroJITx64Impl::JITState, carry_flag), 4);
                        MacroInterpreterImpl interp;
                        const bool sub_family = op == Macro::ALUOperation::Subtract ||
                                                op == Macro::ALUOperation::SubtractWithBorrow;
                        interp.carry_flag =
                            sub_family ? !carry_before : carry_before; // x86 CF after sub is borrow
                        const u32 ia = a == 0 ? 0 : state.registers[a],
                                  ib = b == 0 ? 0 : state.registers[b];
                        const u32 expected = interp.GetALUResult(op, ia, ib);
                        bool ok = r[RESULT.idx] == expected;
                        const bool arithmetic =
                            op == Macro::ALUOperation::Add || op == Macro::ALUOperation::Subtract;
                        const bool carry_op = op == Macro::ALUOperation::AddWithCarry ||
                                              op == Macro::ALUOperation::SubtractWithBorrow;
                        if (carry_op || (arithmetic && !can_skip)) {
                            const u32 want = sub_family ? !interp.carry_flag : interp.carry_flag;
                            ok = ok && carry_after == want;
                        } else {
                            ok = ok && carry_after == carry_before; // logical ops / dead-carry
                                                                    // arithmetic leave it alone
                        }
                        ++cases;
                        if (!ok) {
                            ++mismatches;
                            if (first_reports++ < 6)
                                std::printf("MISMATCH op=%u a=%u b=%u can_skip_carry=%d "
                                            "got=%08x want=%08x carry_after=%u\n",
                                            unsigned(op), a, b, can_skip, r[RESULT.idx], expected,
                                            carry_after);
                        }
                    }
    std::printf("cases=%u mismatches=%u unimplemented_hits=%u\n", cases, mismatches,
                unimplemented_hits);
    if (expect_failures)
        return mismatches ? 0 : 1;
    return mismatches ? 1 : 0;
}
