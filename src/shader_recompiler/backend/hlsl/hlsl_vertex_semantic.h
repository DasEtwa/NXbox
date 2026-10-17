// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace Shader::Backend::HLSL {
// HLSL splits a semantic into its name and numeric index, just like the input layout.
struct VertexSemantic {
    const char* name;
    std::uint32_t index;
};
constexpr VertexSemantic GenericSemantic(std::uint32_t attribute) {
    return {"TEXCOORD", attribute};
}
} // namespace Shader::Backend::HLSL
