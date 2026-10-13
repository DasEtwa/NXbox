// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"
namespace D3D12 {
class ShaderCompiler {
public:
    ShaderCompiler();
    ~ShaderCompiler();
    std::vector<u8> Compile(std::string_view source, const wchar_t* entry, const wchar_t* target,
                            u64 pipeline_hash = 0);

    std::string Disassemble(std::span<const u8> dxil);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12
