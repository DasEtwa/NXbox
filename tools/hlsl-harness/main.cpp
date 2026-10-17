// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/profile.h"
#ifdef NXBOX_HAS_HLSL
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#endif

namespace {
using namespace Shader;
class DumpEnvironment final : public Environment {
public:
  DumpEnvironment() { stage = Stage::Compute; }
  DumpEnvironment(const char *path, unsigned index, u32 address, u32 start)
      : base{address} {
    static constexpr Stage stages[]{
        Stage::VertexA,          Stage::VertexB,  Stage::TessellationControl,
        Stage::TessellationEval, Stage::Geometry, Stage::Fragment};
    if (index >= 6)
      throw std::runtime_error("invalid guest stage index");
    stage = stages[index];
    start_address = start;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
      throw std::runtime_error("cannot open Maxwell dump");
    const auto size = file.tellg();
    if (size <= 0 || size % 8 != 0 || size > 16 * 1024 * 1024)
      throw std::runtime_error(
          "invalid Maxwell dump size (expected aligned words, <=16 MiB)");
    if (u64(base) + u64(size) > u64(std::numeric_limits<u32>::max()) + 1 ||
        start > std::numeric_limits<u32>::max() - sizeof(sph))
      throw std::runtime_error("dump address range overflows");
    code.resize(static_cast<size_t>(size) / 8);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), size);
    if (!file)
      throw std::runtime_error("truncated Maxwell dump");
    if (start < base || u64(start - base) + sizeof(sph) > u64(size))
      throw std::runtime_error(
          "program header is outside dump; capture did not retain header");
    std::memcpy(&sph,
                reinterpret_cast<const char *>(code.data()) + start - base,
                sizeof(sph));
  }
  u64 ReadInstruction(u32 address) override {
    if (address < base || (address - base) % 8 ||
        (address - base) / 8 >= code.size())
      throw std::runtime_error("instruction address outside Maxwell dump");
    return code[(address - base) / 8];
  }
  u32 ReadCbufValue(u32 bank, u32 offset) override {
    throw std::runtime_error(
        "dump lacks constant buffer value: bank=" + std::to_string(bank) +
        " offset=" + std::to_string(offset));
  }
  TextureType ReadTextureType(u32) override {
    throw std::runtime_error("dump lacks texture type");
  }
  TexturePixelFormat ReadTexturePixelFormat(u32) override {
    throw std::runtime_error("dump lacks texture format");
  }
  bool IsTexturePixelFormatInteger(u32) override {
    throw std::runtime_error("dump lacks texture format");
  }
  u32 ReadViewportTransformState() override { return 1; }
  u32 TextureBoundBuffer() const override { return 0; }
  u32 LocalMemorySize() const override {
    const u64 size =
        sph.LocalMemorySize() + sph.common3.shader_local_memory_crs_size;
    if (size > std::numeric_limits<u32>::max())
      throw std::runtime_error("local memory overflow");
    return static_cast<u32>(size);
  }
  u32 SharedMemorySize() const override { return 0; }
  std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
  bool HasHLEMacroState() const override { return false; }
  std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
    return std::nullopt;
  }
  void Dump(u64, u64) override {}

private:
  u32 base{};
  std::vector<u64> code;
};
u32 Number(const char *text) {
  size_t used{};
  const auto value = std::stoull(text, &used, 10);
  if (text[used] || value > std::numeric_limits<u32>::max())
    throw std::runtime_error("invalid integer");
  return static_cast<u32>(value);
}
} // namespace

int main(int argc, char **argv) {
  try {
    static_assert(std::endian::native == std::endian::little,
                  "Maxwell captures require a little-endian host");
    if (argc == 2 && std::string_view(argv[1]) == "--probe") {
#ifdef NXBOX_HAS_HLSL
      std::cout << "HLSL backend available\n";
      return 0;
#else
      std::cout
          << "SKIP: src/shader_recompiler/backend/hlsl is not available yet\n";
      return 77;
#endif
    }
    const bool synthetic =
        argc >= 2 && std::string_view(argv[1]) == "--synthetic";
    if ((synthetic && argc != 4) || (!synthetic && argc != 7 && argc != 10)) {
      std::cerr
          << "usage: hlsl-emitter --synthetic <vs|ps|cs> <output.hlsl|->\n"
             "       hlsl-emitter --dump <binary> <stage-index> <address> "
             "<start> <output.hlsl|-> [<va-binary> <va-address> <va-start>]\n";
      return 1;
    }
    if (!synthetic && std::string_view(argv[1]) != "--dump")
      throw std::runtime_error("expected --dump or --synthetic");
    if (argc == 10 && Number(argv[3]) != 1)
      throw std::runtime_error("VertexA pairing requires a VertexB dump");
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    ObjectPool<Maxwell::Flow::Block> flow_pool;
    DumpEnvironment empty;
    IR::Program program;
    RuntimeInfo runtime{};
    runtime.generic_input_types.fill(AttributeType::Float);
    runtime.color_output_types.fill(AttributeType::Float);
    runtime.input_topology = InputTopology::Triangles;
    runtime.tess_primitive = TessPrimitive::Triangles;
    HostTranslateInfo host{};
    host.support_int64 = true;
    host.support_float64 = true;
    host.support_viewport_index_layer = true;
    host.ApplyDescriptorLimitPolicy();
    if (synthetic) {
      const std::string_view target{argv[2]};
      if (target != "vs" && target != "ps" && target != "cs")
        throw std::runtime_error("unknown synthetic stage");
      program.stage = target == "vs"   ? Stage::VertexB
                      : target == "ps" ? Stage::Fragment
                                       : Stage::Compute;
      program.workgroup_size = {1, 1, 1};
      auto *block = block_pool.Create(inst_pool);
      program.blocks = {block};
      program.post_order_blocks = {block};
      program.syntax_list.push_back(
          {.data = {.block = block},
           .type = IR::AbstractSyntaxNode::Type::Block});
      program.syntax_list.push_back(
          {.type = IR::AbstractSyntaxNode::Type::Return});
      IR::IREmitter ir{*block};
      ir.Prologue();
      if (target == "vs") {
        const IR::F32 input = ir.GetAttribute(IR::Attribute::Generic0X);
        const IR::F32 value{ir.FPAdd(input, ir.Imm32(0.5f))};
        ir.SetAttribute(IR::Attribute::PositionX, value, ir.Imm32(0u));
        ir.SetAttribute(IR::Attribute::PositionY, ir.Imm32(0.0f), ir.Imm32(0u));
        ir.SetAttribute(IR::Attribute::PositionZ, ir.Imm32(0.0f), ir.Imm32(0u));
        ir.SetAttribute(IR::Attribute::PositionW, ir.Imm32(1.0f), ir.Imm32(0u));
      } else if (target == "ps") {
        const IR::F32 input = ir.GetAttribute(IR::Attribute::Generic0X);
        const IR::F32 value{ir.FPMul(input, ir.Imm32(0.5f))};
        for (u32 i = 0; i < 4; ++i)
          ir.SetFragColor(0, i, value);
      } else {
        ir.Barrier();
      }
      ir.Epilogue();
      Optimization::CollectShaderInfoPass(empty, program);
      Optimization::VerificationPass(program);
    } else {
      DumpEnvironment env{argv[2], Number(argv[3]), Number(argv[4]),
                          Number(argv[5])};
      if (env.StartAddress() >
          std::numeric_limits<u32>::max() - sizeof(ProgramHeader))
        throw std::runtime_error("shader start overflows");
      Maxwell::Flow::CFG cfg{env, flow_pool,
                             env.StartAddress() + sizeof(ProgramHeader),
                             Number(argv[3]) == 0};
      program =
          Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
      if (argc == 10) {
        DumpEnvironment va_env{argv[7], 0, Number(argv[8]), Number(argv[9])};
        Maxwell::Flow::CFG va_cfg{va_env, flow_pool,
                                  va_env.StartAddress() + sizeof(ProgramHeader),
                                  true};
        auto va = Maxwell::TranslateProgram(inst_pool, block_pool, va_env,
                                            va_cfg, host);
        program = Maxwell::MergeDualVertexPrograms(va, program, env);
      }
      Maxwell::ConvertLegacyToGeneric(program, runtime);
    }
    const char *output = argv[synthetic ? 3 : 6];
    if (std::string_view(output) == "-") {
      std::cout << IR::DumpProgram(program) << '\n';
      return 0;
    }
#ifdef NXBOX_HAS_HLSL
    Profile profile{};
    Backend::Bindings bindings{};
    const auto source =
        Backend::HLSL::EmitHLSL(profile, runtime, program, bindings);
    if (source.empty())
      throw std::runtime_error("EmitHLSL returned empty source");
    std::ofstream file{output};
    file << source;
    if (!file)
      throw std::runtime_error("cannot write HLSL output");
    return 0;
#else
    std::cout
        << "SKIP: src/shader_recompiler/backend/hlsl is not available yet\n";
    return 77;
#endif
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
