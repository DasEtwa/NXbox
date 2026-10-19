// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "shader_recompiler/backend/hlsl/var_alloc.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/stage.h"

namespace Shader {
struct Info;
struct Profile;
struct RuntimeInfo;
} // namespace Shader

namespace Shader::Backend {
struct Bindings;
}

namespace Shader::IR {
class Inst;
class Value;
enum class Attribute : u64;
struct Program;
} // namespace Shader::IR

namespace Shader::Backend::HLSL {

class EmitContext {
public:
    explicit EmitContext(IR::Program& program, Bindings& bindings, const Profile& profile_,
                         const RuntimeInfo& runtime_info_);

    template <HlslVarType type, typename... Args>
    void Add(const char* format_str, IR::Inst& inst, Args&&... args) {
        const auto var_def{var_alloc.AddDefine(inst, type)};
        code += fmt::format(fmt::runtime(format_str), var_def, std::forward<Args>(args)...);
        code += '\n';
    }

    template <typename... Args>
    void AddU1(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::U1>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddF16x2(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::F16x2>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddU32(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::U32>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddF32(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::F32>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddU64(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::U64>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddF64(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::F64>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddU32x2(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::U32x2>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddF32x2(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::F32x2>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddU32x3(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::U32x3>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddF32x3(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::F32x3>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddU32x4(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::U32x4>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddF32x4(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::F32x4>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddPrecF32(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::PrecF32>(format_str, inst, args...);
    }

    template <typename... Args>
    void AddPrecF64(const char* format_str, IR::Inst& inst, Args&&... args) {
        Add<HlslVarType::PrecF64>(format_str, inst, args...);
    }

    template <typename... Args>
    void Add(const char* format_str, Args&&... args) {
        code += fmt::format(fmt::runtime(format_str), std::forward<Args>(args)...);
        code += '\n';
    }

    std::string header;
    std::string code;
    VarAlloc var_alloc;
    const Info& info;
    const Profile& profile;
    const RuntimeInfo& runtime_info;
    Stage stage{};
    bool uses_cc_carry{};
    bool uses_register_state{};
    bool uses_return_flag{};
    bool uses_shared_wide{};
    std::set<u32> goto_variables;
    std::unordered_set<IR::Inst*> handled_pseudo;
    std::string CbufferLoad(const IR::Value& binding, const IR::Value& offset, u32 delta = 0);
    std::string CbufferLoad(u32 binding, u32 offset);
    std::string Attribute(IR::Attribute attribute, bool output);
    std::string Resource(const IR::Value& index, u32 descriptor, bool image, bool buffer = false);
    // Opens "{ if (found) {" with nx_global_index and nx_global_offset; false without windows.
    bool GlobalMemoryAccess(std::string_view address, u32 bytes);
    std::string Sampler(const IR::Value& index, u32 descriptor);
    struct ResourceDefinition {
        u32 binding;
        u32 count;
    };
    std::vector<ResourceDefinition> textures, texture_buffers, images, image_buffers;
    std::vector<u32> storage_bindings;
    std::string ReturnStatement() const;
    void FinishOutput(std::string_view output = "output");
};
} // namespace Shader::Backend::HLSL
