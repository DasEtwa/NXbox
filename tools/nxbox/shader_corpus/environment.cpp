// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "environment.h"

#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace ShaderCorpus {
namespace {
template <typename T> T Read(std::ifstream &file) {
  static_assert(std::is_trivially_copyable_v<T>);
  T result{};
  file.read(reinterpret_cast<char *>(&result), sizeof(result));
  if (!file) {
    throw std::runtime_error("truncated serialized environment");
  }
  return result;
}
template <typename K, typename V>
void ReadMap(std::ifstream &file, std::map<K, V> &map, u64 count) {
  for (u64 i = 0; i < count; ++i) {
    const K key = Read<K>(file);
    const V value = Read<V>(file);
    if (!map.emplace(key, value).second) {
      throw std::runtime_error("duplicate serialized environment key");
    }
  }
}
template <typename K, typename V>
V Lookup(std::map<K, V> &map, K key, const char *kind, V fallback,
         std::set<std::string> &synthesized) {
  const auto [it, inserted] = map.try_emplace(key, fallback);
  if (inserted) {
    synthesized.insert(std::string(kind) + " key=" + std::to_string(key));
  }
  return it->second;
}
u64 CbufKey(u32 bank, u32 offset) { return (u64{bank} << 32) | offset; }
} // namespace

Environment::Environment(const std::filesystem::path &path) {
  static_assert(std::endian::native == std::endian::little);
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open serialized environment");
  }
  const u64 bytes = Read<u64>(file);
  std::array<u64, 4> counts{};
  for (u64 &count : counts) {
    count = Read<u64>(file);
    if (count > 1048576) {
      throw std::runtime_error("environment map exceeds one million entries");
    }
  }
  local_memory = Read<u32>(file);
  texture_bound = Read<u32>(file);
  start_address = Read<u32>(file);
  base = Read<u32>(file);
  const u32 highest = Read<u32>(file);
  viewport = Read<u32>(file);
  stage = Read<Shader::Stage>(file);
  if (static_cast<u32>(stage) > static_cast<u32>(Shader::Stage::VertexA) ||
      bytes == 0 || bytes % 8 != 0 || bytes > 16 * 1024 * 1024 ||
      base % 8 != 0 || u64{base} + bytes > (u64{1} << 32) ||
      highest != u64{base} + bytes - 8 || start_address % 8 != 0) {
    throw std::runtime_error(
        "invalid serialized environment stage/code/address range");
  }
  code.resize(static_cast<size_t>(bytes / 8));
  file.read(reinterpret_cast<char *>(code.data()),
            static_cast<std::streamsize>(bytes));
  if (!file) {
    throw std::runtime_error("truncated serialized guest code");
  }
  ReadMap(file, textures, counts[0]);
  ReadMap(file, formats, counts[1]);
  ReadMap(file, constants, counts[2]);
  ReadMap(file, replacements, counts[3]);
  for (const auto &[key, value] : textures) {
    if (static_cast<u32>(value) >= Shader::NUM_TEXTURE_TYPES) {
      throw std::runtime_error("invalid cached texture type key=" +
                               std::to_string(key));
    }
  }
  for (const auto &[key, value] : replacements) {
    if (static_cast<u32>(value) >
        static_cast<u32>(Shader::ReplaceConstant::DrawID)) {
      throw std::runtime_error("invalid cached replacement key=" +
                               std::to_string(key));
    }
  }
  if (stage == Shader::Stage::Compute) {
    workgroup = Read<std::array<u32, 3>>(file);
    shared_memory = Read<u32>(file);
  } else {
    sph = Read<Shader::ProgramHeader>(file);
    if (stage == Shader::Stage::Geometry) {
      gp_passthrough_mask = Read<std::array<u32, 8>>(file);
    }
  }
  is_proprietary_driver = texture_bound == 2;
  if (file.peek() != std::ifstream::traits_type::eof()) {
    throw std::runtime_error("trailing bytes in serialized environment");
  }
}

u64 Environment::ReadInstruction(u32 address) {
  if (address < base || (address - base) % 8 ||
      (address - base) / 8 >= code.size()) {
    throw std::runtime_error("instruction outside environment: address=" +
                             std::to_string(address));
  }
  return code[(address - base) / 8];
}
u32 Environment::ReadCbufValue(u32 bank, u32 offset) {
  return Lookup(constants, CbufKey(bank, offset), "constant buffer", u32{0},
                synthesized);
}
Shader::TextureType Environment::ReadTextureType(u32 handle) {
  return Lookup(textures, handle, "texture type", Shader::TextureType::Color2D,
                synthesized);
}
Shader::TexturePixelFormat Environment::ReadTexturePixelFormat(u32 handle) {
  return Lookup(formats, handle, "texture format",
                Shader::TexturePixelFormat::A8B8G8R8_UNORM, synthesized);
}
bool Environment::IsTexturePixelFormatInteger(u32 handle) {
  // Same classification as VideoCore::Surface::IsPixelFormatInteger, without
  // renderer linkage.
  using F = Shader::TexturePixelFormat;
  switch (ReadTexturePixelFormat(handle)) {
  case F::A8B8G8R8_SINT:
  case F::A8B8G8R8_UINT:
  case F::A2B10G10R10_UINT:
  case F::R8_SINT:
  case F::R8_UINT:
  case F::R16G16B16A16_SINT:
  case F::R16G16B16A16_UINT:
  case F::R32G32B32A32_UINT:
  case F::R32G32B32A32_SINT:
  case F::R32G32_SINT:
  case F::R16_UINT:
  case F::R16_SINT:
  case F::R16G16_UINT:
  case F::R16G16_SINT:
  case F::R8G8_SINT:
  case F::R8G8_UINT:
  case F::R32G32_UINT:
  case F::R32_UINT:
  case F::R32_SINT:
    return true;
  default:
    return false;
  }
}
std::optional<Shader::ReplaceConstant>
Environment::GetReplaceConstBuffer(u32 bank, u32 offset) {
  const auto it = replacements.find(CbufKey(bank, offset));
  return it == replacements.end() ? std::nullopt : std::optional{it->second};
}
} // namespace ShaderCorpus
