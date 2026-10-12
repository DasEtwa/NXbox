// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/type.h"

namespace Shader::IR {
class Inst;
class Value;
} // namespace Shader::IR
namespace Shader::Backend::HLSL {
enum class HlslVarType : u32 {
    U1,
    F16x2,
    U32,
    F32,
    U64,
    F64,
    F64x2,
    F64x3,
    F64x4,
    U32x2,
    F32x2,
    U32x3,
    F32x3,
    U32x4,
    F32x4,
    PrecF32,
    PrecF64,
    Void,
};

// Values keep unique names across structured branches and loop back edges.
// Unlike the GLSL allocator this does not consume the IR's usage counts.
class VarAlloc {
public:
    std::string Define(IR::Inst& inst, HlslVarType type);
    std::string Define(IR::Inst& inst, IR::Type type);
    std::string AddDefine(IR::Inst& inst, HlslVarType type) {
        return Define(inst, type);
    }
    std::string PhiDefine(IR::Inst& inst, IR::Type type) {
        return Define(inst, type);
    }
    std::string Consume(const IR::Value& value);
    std::string ConsumeInst(IR::Inst& inst);
    std::string GetHlslType(HlslVarType type) const;
    std::string GetHlslType(IR::Type type) const;
    std::string Declarations() const;

private:
    struct Variable {
        HlslVarType type;
        std::string name;
    };
    std::unordered_map<IR::Inst*, size_t> names;
    std::vector<Variable> variables;
};
} // namespace Shader::Backend::HLSL
