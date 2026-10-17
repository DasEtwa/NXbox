// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <stdexcept>

#include "environment.h"

int main(int argc, char **argv) {
  try {
    if (argc != 2) {
      throw std::runtime_error("expected synthetic environment path");
    }
    ShaderCorpus::Environment env{argv[1]};
    const auto require = [](bool condition) {
      if (!condition) {
        throw std::runtime_error("environment default regression");
      }
    };
    // Present entries must survive unchanged, with no default recorded.
    require(env.ReadTextureType(4) == Shader::TextureType::Color3D);
    require(env.ReadTexturePixelFormat(4) ==
            Shader::TexturePixelFormat::A8B8G8R8_UINT);
    require(env.IsTexturePixelFormatInteger(4));
    require(env.ReadCbufValue(2, 16) == 123);
    require(env.SynthesizedKeys().empty());
    for (int i = 0; i < 2; ++i) {
      require(env.ReadTextureType(7) == Shader::TextureType::Color2D);
      require(env.ReadTexturePixelFormat(7) ==
              Shader::TexturePixelFormat::A8B8G8R8_UNORM);
      require(!env.IsTexturePixelFormatInteger(7));
      require(env.ReadCbufValue(2, 32) == 0);
      require(!env.GetReplaceConstBuffer(2, 32));
    }
    require(env.SynthesizedKeys() ==
            std::set<std::string>{"texture type key=7", "texture format key=7",
                                  "constant buffer key=8589934624"});
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
