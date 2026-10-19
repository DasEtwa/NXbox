// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production JIT skip predicate without an x86 executable-code backend.
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace Macro {
// ALU_ENUM
}
bool CanSkip(Macro::ALUOperation operation, u32 a, u32 b, bool enabled, bool dead_carry) {
    const bool is_a_zero = a == 0;
    const bool is_b_zero = b == 0;
    const struct {
        bool zero_reg_skip, can_skip_carry;
    } optimizer{enabled, dead_carry};
    const struct {
        Macro::ALUOperation alu_operation;
    } opcode{operation};
    // PREDICATE
    return zero_reg_skip;
}
int main() {
    std::mt19937 random{0x12345678};
    for (u32 operation = 0; operation < 32; ++operation)
        for (u32 a = 0; a < 8; ++a)
            for (u32 b = 0; b < 8; ++b)
                for (bool enabled : {false, true})
                    for (bool dead_carry : {false, true}) {
                        const auto op = static_cast<Macro::ALUOperation>(operation);
                        const bool skip = CanSkip(op, a, b, enabled, dead_carry);
                        const bool logical_identity = operation == 8 || operation == 9;
                        const bool arithmetic_identity = operation == 0 || operation == 2;
                        assert(skip == (enabled && a != 0 && b == 0 &&
                                        (logical_identity || (dead_carry && arithmetic_identity))));
                        if (!skip)
                            continue;
                        for (u32 value : {0u, 1u, 0x80000000u, 0xFFFFFFFFu, random()}) {
                            // The elided instruction leaves RESULT == src_a. These are the
                            // only legal identities, and arithmetic carry must be dead.
                            u32 result{};
                            switch (op) {
                            case Macro::ALUOperation::Add:
                                result = value + 0u;
                                break;
                            case Macro::ALUOperation::Subtract:
                                result = value - 0u;
                                break;
                            case Macro::ALUOperation::Xor:
                                result = value ^ 0u;
                                break;
                            case Macro::ALUOperation::Or:
                                result = value | 0u;
                                break;
                            default:
                                assert(false);
                            }
                            assert(result == value);
                            assert(!arithmetic_identity || dead_carry);
                        }
                    }
}
