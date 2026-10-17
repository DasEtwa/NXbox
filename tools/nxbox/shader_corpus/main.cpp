// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>

#include "environment.h"
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace {
// Match the native cache ABI without linking OpenGL or Vulkan renderer code.
struct GraphicsKey {
  std::array<u64, 6> hashes;
  u32 raw;
  std::array<u32, 3> padding;
  VideoCommon::TransformFeedbackState xfb;
};
struct VulkanKey {
  std::array<u64, 6> hashes;
  Vulkan::FixedPipelineState state;
};
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view{argv[1]} == "--layout") {
      std::cout << "gl=" << sizeof(GraphicsKey) << " vk=" << sizeof(VulkanKey)
                << '\n';
      return 0;
    }
    if (argc != 3 && argc != 4) {
      std::cerr << "usage: nxbox-shader-corpus <environment.bin> <output.hlsl> "
                   "[vertex-a.bin]\n";
      return 2;
    }
    ShaderCorpus::Environment env{std::filesystem::path{argv[1]}};
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::HostTranslateInfo host{};
    host.support_float64 = true;
    host.support_int64 = true;
    host.support_viewport_index_layer = true;
    host.min_ssbo_alignment = 16;
    host.ApplyDescriptorLimitPolicy();
    const auto translate = [&](ShaderCorpus::Environment &input) {
      const u32 header = input.ShaderStage() == Shader::Stage::Compute
                             ? 0
                             : sizeof(Shader::ProgramHeader);
      if (input.StartAddress() > std::numeric_limits<u32>::max() - header) {
        throw std::runtime_error("shader entry point overflows");
      }
      Shader::Maxwell::Flow::CFG cfg{
          input, flow_pool, input.StartAddress() + header,
          input.ShaderStage() == Shader::Stage::VertexA};
      return Shader::Maxwell::TranslateProgram(inst_pool, block_pool, input,
                                               cfg, host);
    };
    auto program = translate(env);
    std::set<std::string> defaults;
    if (argc == 4) {
      ShaderCorpus::Environment va{std::filesystem::path{argv[3]}};
      if (env.ShaderStage() != Shader::Stage::VertexB ||
          va.ShaderStage() != Shader::Stage::VertexA) {
        throw std::runtime_error(
            "dual vertex pairing requires VertexA and VertexB");
      }
      auto first = translate(va);
      program = Shader::Maxwell::MergeDualVertexPrograms(first, program, env);
      for (const auto &key : va.SynthesizedKeys()) {
        defaults.insert("va " + key);
      }
    }
    Shader::RuntimeInfo runtime{};
    runtime.generic_input_types.fill(Shader::AttributeType::Float);
    runtime.color_output_types.fill(Shader::AttributeType::Float);
    runtime.input_topology = Shader::InputTopology::Triangles;
    runtime.tess_primitive = Shader::TessPrimitive::Triangles;
    Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
    // Isolated fragment validation needs an input signature for every consumed
    // varying. This declares the interface; it does not reconstruct the
    // preceding pipeline stage.
    runtime.previous_stage_stores = program.info.loads;
    Shader::Profile profile{};
    profile.support_int64 = true;
    profile.support_vertex_instance_id = true;
    profile.support_vote = true;
    profile.support_quad_shuffles = true;
    profile.support_derivative_control = true;
    profile.support_viewport_index_layer_non_geometry = true;
    profile.max_user_clip_distances = 8;
    profile.min_ssbo_alignment = 16;
    Shader::Backend::Bindings bindings{};
    const auto source =
        Shader::Backend::HLSL::EmitHLSL(profile, runtime, program, bindings);
    if (source.empty()) {
      throw std::runtime_error("EmitHLSL returned empty source");
    }
    std::ofstream output{std::filesystem::path{argv[2]}};
    output << source;
    if (!output) {
      throw std::runtime_error("cannot write HLSL output");
    }
    for (const auto &key : env.SynthesizedKeys()) {
      defaults.insert(std::string{env.ShaderStage() == Shader::Stage::VertexB
                                      ? "vb "
                                      : "stage "} +
                      key);
    }
    for (const auto &key : defaults) {
      std::cout << "DEFAULT: " << key << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "FAIL: non-standard exception in shader translation\n";
  }
  return 1;
}
