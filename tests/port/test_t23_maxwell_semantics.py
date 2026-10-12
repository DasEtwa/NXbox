"""Differential-test real Maxwell translator IR against independent host oracles."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CXX_SOURCE = r"""
#include "shader_recompiler/frontend/ir/opcodes.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include <array>
#include <bit>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace IR = Shader::IR;
using Visitor = Shader::Maxwell::TranslatorVisitor;
struct Environment final : Shader::Environment {
  u64 ReadInstruction(u32) override { return 0; }
  u32 ReadCbufValue(u32, u32) override { return 0; }
  Shader::TextureType ReadTextureType(u32) override {
    return Shader::TextureType::Color2D;
  }
  Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
    return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
  }
  bool IsTexturePixelFormatInteger(u32) override { return false; }
  u32 ReadViewportTransformState() override { return 0; }
  u32 TextureBoundBuffer() const override { return 2; }
  u32 LocalMemorySize() const override { return 0; }
  u32 SharedMemorySize() const override { return 0; }
  std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
  bool HasHLEMacroState() const override { return false; }
  std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32,
                                                               u32) override {
    return {};
  }
  void Dump(u64, u64) override {}
};
struct Evaluation {
  std::array<u32, 256> registers{};
  u32 cbuf{};
  std::unordered_map<const IR::Inst *, u32> values;
  u32 Read(IR::Value value) {
    if (value.IsImmediate()) {
      return value.Type() == IR::Type::U1 ? value.U1() : value.U32();
    }
    return values.at(value.Inst());
  }
  void Run(const IR::Block &block) {
    registers[255] = 0;
    for (const auto &inst : block.Instructions()) {
      const auto arg = [&](size_t index) { return Read(inst.Arg(index)); };
      u32 result{};
      switch (inst.GetOpcode()) {
      case IR::Opcode::GetRegister:
        result = registers[IR::RegIndex(inst.Arg(0).Reg())];
        break;
      case IR::Opcode::SetRegister:
        if (inst.Arg(0).Reg() != IR::Reg::RZ)
          registers[IR::RegIndex(inst.Arg(0).Reg())] = arg(1);
        break;
      case IR::Opcode::GetCbufU32:
        if (arg(0) != 1 || arg(1) != 16)
          throw std::runtime_error("Incorrect cbuf address");
        result = cbuf;
        break;
      case IR::Opcode::IAdd32:
        result = arg(0) + arg(1);
        break;
      case IR::Opcode::ISub32:
        result = arg(0) - arg(1);
        break;
      case IR::Opcode::IMul32:
        result = arg(0) * arg(1);
        break;
      case IR::Opcode::BitwiseAnd32:
        result = arg(0) & arg(1);
        break;
      case IR::Opcode::BitwiseOr32:
        result = arg(0) | arg(1);
        break;
      case IR::Opcode::ShiftLeftLogical32:
        result = arg(1) < 32 ? arg(0) << arg(1) : 0;
        break;
      case IR::Opcode::ShiftRightLogical32:
        result = arg(1) < 32 ? arg(0) >> arg(1) : 0;
        break;
      case IR::Opcode::BitFieldUExtract:
        result = (arg(0) >> arg(1)) & ((1u << arg(2)) - 1);
        break;
      case IR::Opcode::IEqual:
        result = arg(0) == arg(1);
        break;
      case IR::Opcode::SLessThan:
        result = std::bit_cast<s32>(arg(0)) < std::bit_cast<s32>(arg(1));
        break;
      case IR::Opcode::UMin32:
        result = std::min(arg(0), arg(1));
        break;
      case IR::Opcode::UMax32:
        result = std::max(arg(0), arg(1));
        break;
      case IR::Opcode::SelectU32:
        result = arg(0) ? arg(1) : arg(2);
        break;
      default:
        throw std::runtime_error(std::string(IR::NameOf(inst.GetOpcode())));
      }
      values[&inst] = result;
    }
  }
};
using Method = void (Visitor::*)(u64);
size_t cases{};
void Check(Method method, u64 insn, u32 a, u32 b, u32 c, u32 cbuf,
           u32 expected) {
  Shader::ObjectPool<IR::Inst> pool;
  IR::Block block{pool};
  Environment env;
  Visitor visitor{env, block};
  (visitor.*method)(insn);
  Evaluation eval;
  eval.registers[0] = a; // Exercise destination/source aliasing.
  eval.registers[1] = a;
  eval.registers[2] = b;
  eval.registers[3] = c;
  eval.cbuf = cbuf;
  eval.Run(block);
  if (eval.registers[0] != expected) {
    throw std::runtime_error("IR mismatch: " + std::to_string(insn) + ": " +
                             std::to_string(eval.registers[0]) +
                             " != " + std::to_string(expected));
  }
  ++cases;
}
u32 Permute(u32 a, u32 b, u32 c, unsigned mode) {
  // Independent byte-array oracle; specialized selectors are the PTX table.
  constexpr unsigned table[6][4][4] = {
      {{0, 1, 2, 3}, {1, 2, 3, 4}, {2, 3, 4, 5}, {3, 4, 5, 6}},
      {{0, 7, 6, 5}, {1, 0, 7, 6}, {2, 1, 0, 7}, {3, 2, 1, 0}},
      {{0, 0, 0, 0}, {1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}},
      {{0, 1, 2, 3}, {1, 1, 2, 3}, {2, 2, 2, 3}, {3, 3, 3, 3}},
      {{0, 0, 0, 0}, {0, 1, 1, 1}, {0, 1, 2, 2}, {0, 1, 2, 3}},
      {{0, 1, 0, 1}, {2, 3, 2, 3}, {0, 1, 0, 1}, {2, 3, 2, 3}},
  };
  std::array<u8, 8> bytes{};
  for (unsigned i = 0; i < 4; ++i) {
    bytes[i] = static_cast<u8>(a >> (8 * i));
    bytes[i + 4] = static_cast<u8>(b >> (8 * i));
  }
  u32 result{};
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned selector =
        mode ? table[mode - 1][c & 3][i] : (c >> (i * 4)) & 15;
    const u8 byte = mode || !(selector & 8)
                        ? bytes[selector & 7]
                        : (bytes[selector & 7] & 128 ? 255 : 0);
    result |= static_cast<u32>(byte) << (8 * i);
  }
  return result;
}
u32 Product(u32 a, u32 b, bool high, bool signed_a, bool signed_b) {
  // Native 64-bit arithmetic, independent of the translator's 16-bit limbs.
  const s64 left =
      signed_a ? static_cast<s64>(std::bit_cast<s32>(a)) : static_cast<s64>(a);
  const s64 right =
      signed_b ? static_cast<s64>(std::bit_cast<s32>(b)) : static_cast<s64>(b);
  const u64 product = static_cast<u64>(left) * static_cast<u64>(right);
  return static_cast<u32>(high ? product >> 32 : product);
}
int main() {
  std::mt19937 random{0x23a11};
  constexpr u64 regs = (1ull << 8) | (2ull << 20) | (3ull << 39);
  constexpr u64 cbuf_address = (1ull << 34) | (4ull << 20);
  // Known SASS PRMT immediate selector: pack two halfwords from distinct sources.
  Check(&Visitor::PRMT_imm,
        0x36c0000000000000ull | (1ull << 8) | (3ull << 39) | (0x5410ull << 20),
        0x44332211, 0, 0x88776655, 0, 0x66552211);
  // Exhaust all generic selector values, with bytes containing both signs.
  for (u32 selector = 0; selector < 65536; ++selector) {
    Check(&Visitor::PRMT_reg, 0x5bc0000000000000ull | regs, 0x807f00ff,
          selector, 0x017e81fe, 0,
          Permute(0x807f00ff, 0x017e81fe, selector, 0));
  }
  for (unsigned mode = 0; mode < 7; ++mode) {
    for (unsigned trial = 0; trial < 1024; ++trial) {
      const u32 a = random(), b = random(), c = random();
      const u64 bits = static_cast<u64>(mode) << 48;
      Check(&Visitor::PRMT_reg, 0x5bc0000000000000ull | bits | regs, a, c, b, 0,
            Permute(a, b, c, mode));
      Check(&Visitor::PRMT_cr,
            0x4bc0000000000000ull | bits | (1ull << 8) | (3ull << 39) |
                cbuf_address,
            a, 0, b, c, Permute(a, b, c, mode));
      Check(&Visitor::PRMT_rc,
            0x53c0000000000000ull | bits | (1ull << 8) | (3ull << 39) |
                cbuf_address,
            a, 0, c, b, Permute(a, b, c, mode));
      const s32 immediate = static_cast<s32>(random() % 1048576) - 524288;
      const u64 encoded =
          (static_cast<u64>(static_cast<u32>(immediate) & 0x7ffff) << 20) |
          (immediate < 0 ? 1ull << 56 : 0);
      Check(&Visitor::PRMT_imm,
            0x36c0000000000000ull | bits | (1ull << 8) | (3ull << 39) | encoded,
            a, 0, b, 0, Permute(a, b, static_cast<u32>(immediate), mode));
    }
  }
  constexpr std::array<u32, 10> edges{
      0,       1,          2,          0x7fff,     0xffff,
      0x10000, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff};
  for (unsigned flags = 0; flags < 8; ++flags) {
    const bool high = flags & 1, signed_a = flags & 2, signed_b = flags & 4;
    const u64 mul_flags = (high ? 1ull << 39 : 0) |
                          (signed_a ? 1ull << 40 : 0) |
                          (signed_b ? 1ull << 41 : 0);
    const u64 mad_flags = (high ? 1ull << 54 : 0) |
                          (signed_a ? 1ull << 48 : 0) |
                          (signed_b ? 1ull << 53 : 0);
    for (unsigned trial = 0; trial < 2048; ++trial) {
      const u32 a = trial < 100 ? edges[trial / 10] : random();
      const u32 b = trial < 100 ? edges[trial % 10] : random();
      const u32 c = random();
      Check(&Visitor::IMUL_reg,
            0x5c38000000000000ull | mul_flags | (regs & ~(255ull << 39)), a, b,
            c, 0, Product(a, b, high, signed_a, signed_b));
      Check(&Visitor::IMUL_cbuf,
            0x4c38000000000000ull | mul_flags | (1ull << 8) | cbuf_address, a,
            0, 0, b, Product(a, b, high, signed_a, signed_b));
      const s32 immediate = static_cast<s32>(random() % 1048576) - 524288;
      const u64 encoded =
          (static_cast<u64>(static_cast<u32>(immediate) & 0x7ffff) << 20) |
          (immediate < 0 ? 1ull << 56 : 0);
      Check(&Visitor::IMUL_imm,
            0x3838000000000000ull | mul_flags | (1ull << 8) | encoded, a, 0, 0,
            0,
            Product(a, static_cast<u32>(immediate), high, signed_a, signed_b));
      const u64 wide_flags = (high ? 1ull << 53 : 0) |
                             (signed_a ? 1ull << 54 : 0) |
                             (signed_b ? 1ull << 55 : 0);
      Check(&Visitor::IMUL32I,
            0x1f00000000000000ull | wide_flags | (1ull << 8) |
                (static_cast<u64>(b) << 20),
            a, 0, 0, 0, Product(a, b, high, signed_a, signed_b));
      const u64 immediate_mad_flags = (high ? 1ull << 53 : 0) |
                                      (signed_a ? 1ull << 54 : 0) |
                                      (signed_b ? 1ull << 57 : 0);
      Check(&Visitor::IMAD32I,
            0x1000000000000000ull | immediate_mad_flags | (1ull << 8) |
                (static_cast<u64>(b) << 20),
            a, 0, 0, 0, Product(a, b, high, signed_a, signed_b) + a);
      Check(&Visitor::IMAD_reg, 0x5a00000000000000ull | mad_flags | regs, a, b,
            c, 0, Product(a, b, high, signed_a, signed_b) + c);
      Check(&Visitor::IMAD_cr,
            0x4a00000000000000ull | mad_flags | (1ull << 8) | (3ull << 39) |
                cbuf_address,
            a, 0, c, b, Product(a, b, high, signed_a, signed_b) + c);
      Check(&Visitor::IMAD_rc,
            0x5200000000000000ull | mad_flags | (1ull << 8) | (3ull << 39) |
                cbuf_address,
            a, 0, b, c, Product(a, b, high, signed_a, signed_b) + c);
      Check(&Visitor::IMAD_imm,
            0x3400000000000000ull | mad_flags | (1ull << 8) | (3ull << 39) |
                encoded,
            a, 0, c, 0,
            Product(a, static_cast<u32>(immediate), high, signed_a, signed_b) +
                c);
      // Destination aliases operand A, so GetRegister must precede SetRegister.
      Check(&Visitor::IMUL_reg,
            0x5c38000000000000ull | mul_flags | (2ull << 20), a, b, 0, 0,
            Product(a, b, high, signed_a, signed_b));
    }
  }
  for (unsigned mode = 7; mode < 16; ++mode) {
    try {
      Check(&Visitor::PRMT_reg,
            0x5bc0000000000000ull | (static_cast<u64>(mode) << 48) | regs, 0, 0,
            0, 0, 0);
      throw std::runtime_error("Reserved PRMT mode accepted");
    } catch (const Shader::NotImplementedException &) {
    }
  }
  std::cout << "Differential cases: " << cases << '\n';
}
"""


class MaxwellSemanticsTests(unittest.TestCase):
    def test_native_ir_differential(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if not compiler:
            self.skipTest("A C++20 host compiler is required")
        candidates = [
            Path(os.environ.get("NXBOX_T23_INCLUDE", "/opt/homebrew/include")),
            Path("/usr/include"),
        ]
        includes = next(
            (
                path
                for path in candidates
                if (path / "boost/unordered/unordered_flat_map.hpp").is_file()
                and (path / "fmt/format.h").is_file()
            ),
            None,
        )
        if includes is None:
            self.skipTest("Boost >= 1.81 and fmt headers are required (NXBOX_T23_INCLUDE)")
        fmt_include = Path(os.environ.get("NXBOX_T23_FMT_INCLUDE", str(includes)))
        recompiler = ROOT / "src/shader_recompiler"
        sources = [
            recompiler / "frontend/ir" / name
            for name in (
                "basic_block.cpp",
                "condition.cpp",
                "ir_emitter.cpp",
                "microinstruction.cpp",
                "opcodes.cpp",
                "type.cpp",
                "value.cpp",
                "patch.cpp",
            )
        ]
        sources += [
            recompiler / "frontend/maxwell/opcodes.cpp",
            recompiler / "frontend/maxwell/translate/impl/impl.cpp",
            recompiler / "frontend/maxwell/translate/impl/not_implemented.cpp",
            ROOT / "tools/hlsl-harness/host_support.cpp",
        ]
        with tempfile.TemporaryDirectory(prefix="nxbox-t23-") as directory:
            path = Path(directory)
            native = path / "semantics.cpp"
            native.write_text(CXX_SOURCE)
            binary = path / "semantics"
            args = [
                compiler,
                "-std=c++20",
                "-O1",
                "-DFMT_HEADER_ONLY",
                "-DFMT_USE_CONSTEVAL=0",
                f"-I{ROOT / 'tools/hlsl-harness/host'}",
                f"-I{ROOT / 'src'}",
                f"-I{fmt_include}",
                f"-I{includes}",
            ]
            if "clang" in compiler or os.uname().sysname == "Darwin":
                args += ["-fbracket-depth=1024"]
            built = subprocess.run(
                [*args, str(native), *map(str, sources), "-o", str(binary)],
                capture_output=True,
                text=True,
                timeout=180,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Differential cases: 258049", result.stdout)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
