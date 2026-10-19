// SPDX-License-Identifier: GPL-3.0-or-later
// Production emitters executed by a portable x86 instruction model, adapted
// from review.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <vector>
using u32 = std::uint32_t;
using s32 = std::int32_t;
#define UNIMPLEMENTED_MSG(...)                                                                     \
    do {                                                                                           \
    } while (false)
namespace Core {
struct System {};
} // namespace Core
namespace Macro {
enum class ResultOperation : u32 { IgnoreAndFetch = 0, Move = 1, MoveAndSetMethod = 2 };
struct Opcode {
    u32 src_a, dst;
    s32 immediate;
    ResultOperation result_operation;
};
} // namespace Macro
namespace Xbyak {
struct Reg64;
struct Label {};
struct Reg32 {
    int idx{-1};
    Reg32() = default;
    constexpr Reg32(int i) : idx(i) {}
    Reg64 cvt64() const;
};
struct Reg64 {
    int idx;
};
inline Reg64 Reg32::cvt64() const {
    return {idx};
}
} // namespace Xbyak
namespace Engines {
struct Maxwell3D {
    struct Regs {
        static constexpr u32 NUM_REGS = 0xE00;
        std::array<u32, NUM_REGS> reg_array;
    } regs;
};
} // namespace Engines
struct Addr {
    long off;
    int base{3}, index{-1};
    long scale{};
};
struct Index {
    int reg;
    long scale;
};
Index operator*(Xbyak::Reg64 r, unsigned long scale) {
    return {r.idx, static_cast<long>(scale)};
}
Addr operator+(Addr a, Index i) {
    a.index = i.reg;
    a.scale = i.scale;
    return a;
}
template <class T>
Addr operator+(Xbyak::Reg64 r, T o) {
    return {static_cast<long>(o), r.idx, -1, 0};
}
template <class T>
Addr operator+(Addr a, T o) {
    a.off += static_cast<long>(o);
    return a;
}
struct Mem {
    Addr addr;
    int size;
};
struct MemSpace {
    int size;
    Mem operator[](Addr a) const {
        return {a, size};
    }
};
constexpr Xbyak::Reg64 STATE{3};
constexpr Xbyak::Reg32 RESULT{10};
struct Insn {
    enum K { AddImm, SubImm, Inc, MovImm, XorSelf, Load, Read, Pointer } k;
    int d;
    long imm;
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
    std::optional<Macro::Opcode> next_opcode{};
    std::vector<Insn> code;
    MemSpace dword{4}, qword{8};
    const Xbyak::Reg64 rax{0};
    void cmp(Xbyak::Reg32, u32) {}
    void jb(Xbyak::Label&) {}
    void int3() {
        assert(false);
    }
    void L(Xbyak::Label&) {}
    void add(Xbyak::Reg32 d, long imm) {
        code.push_back({Insn::AddImm, d.idx, imm});
    }
    void sub(Xbyak::Reg32 d, long imm) {
        code.push_back({Insn::SubImm, d.idx, imm});
    }
    void inc(Xbyak::Reg32 d) {
        code.push_back({Insn::Inc, d.idx, 0});
    }
    void mov(Xbyak::Reg32 d, long imm) {
        code.push_back({Insn::MovImm, d.idx, imm});
    }
    void mov(Xbyak::Reg64 d, Mem m) {
        assert(d.idx == 0 && m.size == 8 && m.addr.base == STATE.idx);
        assert(m.addr.off == offsetof(JITState, maxwell3d));
        code.push_back({Insn::Pointer, d.idx, m.addr.off});
    }
    void mov(Xbyak::Reg32 d, Mem m) {
        assert(m.size == 4);
        if (m.addr.index >= 0) {
            assert(m.addr.base == rax.idx && m.addr.index == RESULT.idx && m.addr.scale == 4);
            assert(m.addr.off == offsetof(Engines::Maxwell3D, regs) +
                                     offsetof(Engines::Maxwell3D::Regs, reg_array));
            code.push_back({Insn::Read, d.idx, 0});
        } else {
            assert(m.addr.base == STATE.idx);
            code.push_back({Insn::Load, d.idx, m.addr.off});
        }
    }
    void xor_(Xbyak::Reg32 d, Xbyak::Reg32) {
        code.push_back({Insn::XorSelf, d.idx, 0});
    }
    void Compile_ProcessResult(Core::System&, Macro::ResultOperation, u32) {}
    void Compile_Read(Core::System& system, Macro::Opcode opcode);
    void Compile_AddImmediate(Core::System& system, Macro::Opcode opcode);
    Xbyak::Reg32 Compile_GetRegister(u32 index, Xbyak::Reg32 dst);
};
// PRODUCTION
int main() {
    std::mt19937 rng{7};
    unsigned cases = 0, bad = 0;
    for (bool read : {false, true})
        for (bool zero_skip : {false, true})
            for (u32 a = 0; a < 8; ++a)
                for (s32 imm : {-3, -2, -1, 0, 1, 2, 3, 4, 100, 0x1ffff, -0x20000})
                    for (int trial = 0; trial < 5; ++trial) {
                        MacroJITx64Impl jit;
                        jit.optimizer.zero_reg_skip = zero_skip;
                        jit.optimizer.skip_dummy_addimmediate = true;
                        jit.optimizer.optimize_for_method_move = true;
                        Core::System sys;
                        Macro::Opcode op{a, 1, imm, Macro::ResultOperation::Move};
                        if (read)
                            jit.Compile_Read(sys, op);
                        else
                            jit.Compile_AddImmediate(sys, op);
                        MacroJITx64Impl::JITState st{};
                        for (u32 r = 0; r < 8; ++r)
                            st.registers[r] = rng();
                        st.registers[0] = rng();
                        if (read && a != 0)
                            st.registers[a] = trial < 3 ? 100u : rng();
                        Engines::Maxwell3D engine;
                        for (auto& value : engine.regs.reg_array)
                            value = rng();
                        st.maxwell3d = &engine;
                        bool pointer_loaded = false;
                        u32 read_index = 0;
                        // Model invalid guest register addresses with a deterministic
                        // sentinel; no host out-of-bounds reads, and still compare the full
                        // computed address.
                        const auto fetch = [&](u32 index) {
                            return index < engine.regs.NUM_REGS ? engine.regs.reg_array[index]
                                                                : (index ^ 0x9e3779b9u);
                        };
                        u32 res = rng();
                        for (const auto& i : jit.code)
                            switch (i.k) {
                            case Insn::Pointer:
                                pointer_loaded = true;
                                break;
                            case Insn::Read:
                                assert(pointer_loaded);
                                read_index = res;
                                res = fetch(res);
                                break;
                            case Insn::AddImm:
                                res += static_cast<u32>(i.imm);
                                break;
                            case Insn::SubImm:
                                res -= static_cast<u32>(i.imm);
                                break;
                            case Insn::Inc:
                                ++res;
                                break;
                            case Insn::MovImm:
                                res = static_cast<u32>(i.imm);
                                break;
                            case Insn::XorSelf:
                                res = 0;
                                break;
                            case Insn::Load: {
                                u32 v;
                                std::memcpy(&v, reinterpret_cast<unsigned char*>(&st) + i.imm, 4);
                                res = v;
                                break;
                            }
                            }
                        const u32 address = (a == 0 ? 0u : st.registers[a]) + static_cast<u32>(imm);
                        const u32 want = read ? fetch(address) : address;
                        if (read)
                            assert(read_index == address);
                        ++cases;
                        if (res != want) {
                            ++bad;
                            if (bad <= 4)
                                std::printf("MISMATCH immediate emitter src_a=%u immediate=%d "
                                            "got=%08x want=%08x\n",
                                            a, imm, res, want);
                        }
                    }
    std::printf("cases=%u mismatches=%u\n", cases, bad);
    return bad ? 1 : 0;
}
