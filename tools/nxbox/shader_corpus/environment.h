// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <map>
#include <vector>

#include "shader_recompiler/environment.h"

namespace ShaderCorpus {
// The wire representation is GenericEnvironment::Serialize, not native map
// memory.
class Environment final : public Shader::Environment {
public:
  explicit Environment(const std::filesystem::path &path);
  u64 ReadInstruction(u32 address) override;
  u32 ReadCbufValue(u32 bank, u32 offset) override;
  Shader::TextureType ReadTextureType(u32 handle) override;
  Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;
  bool IsTexturePixelFormatInteger(u32 handle) override;
  u32 ReadViewportTransformState() override { return viewport; }
  u32 TextureBoundBuffer() const override { return texture_bound; }
  u32 LocalMemorySize() const override { return local_memory; }
  u32 SharedMemorySize() const override { return shared_memory; }
  std::array<u32, 3> WorkgroupSize() const override { return workgroup; }
  bool HasHLEMacroState() const override { return !replacements.empty(); }
  std::optional<Shader::ReplaceConstant>
  GetReplaceConstBuffer(u32 bank, u32 offset) override;
  void Dump(u64, u64) override {}

private:
  std::vector<u64> code;
  std::map<u32, Shader::TextureType> textures;
  std::map<u32, Shader::TexturePixelFormat> formats;
  std::map<u64, u32> constants;
  std::map<u64, Shader::ReplaceConstant> replacements;
  std::array<u32, 3> workgroup{};
  u32 local_memory{}, texture_bound{}, base{}, viewport{}, shared_memory{};
};
} // namespace ShaderCorpus
