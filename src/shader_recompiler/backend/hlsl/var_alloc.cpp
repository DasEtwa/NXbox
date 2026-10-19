// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <bit>
#include <fmt/format.h>
#include "shader_recompiler/backend/hlsl/var_alloc.h"
#include "shader_recompiler/backend/hlsl/hlsl_source.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Backend::HLSL {
namespace {
HlslVarType RegType(IR::Type type) {
    switch (type) {
    case IR::Type::U1:
        return HlslVarType::U1;
    case IR::Type::U8:
    case IR::Type::U16:
    case IR::Type::U32:
        return HlslVarType::U32;
    case IR::Type::U64:
        return HlslVarType::U64;
    case IR::Type::F16:
    case IR::Type::F32:
        return HlslVarType::F32;
    case IR::Type::F64:
        return HlslVarType::F64;
    case IR::Type::F64x2:
        return HlslVarType::F64x2;
    case IR::Type::F64x3:
        return HlslVarType::F64x3;
    case IR::Type::F64x4:
        return HlslVarType::F64x4;
    case IR::Type::F16x2:
        return HlslVarType::F16x2;
    case IR::Type::U32x2:
        return HlslVarType::U32x2;
    case IR::Type::U32x3:
        return HlslVarType::U32x3;
    case IR::Type::U32x4:
        return HlslVarType::U32x4;
    case IR::Type::F32x2:
        return HlslVarType::F32x2;
    case IR::Type::F16x3:
    case IR::Type::F32x3:
        return HlslVarType::F32x3;
    case IR::Type::F16x4:
    case IR::Type::F32x4:
        return HlslVarType::F32x4;
    default:
        throw NotImplementedException("HLSL value type {}", type);
    }
}
} // namespace
std::string VarAlloc::Define(IR::Inst& inst, HlslVarType type) {
    if (const auto it = names.find(&inst); it != names.end())
        return variables[it->second].name;
    const auto index = variables.size();
    names.emplace(&inst, index);
    variables.push_back({type, fmt::format("v{}", index)});
    return variables.back().name;
}
std::string VarAlloc::Define(IR::Inst& inst, IR::Type type) {
    return Define(inst, RegType(type));
}
std::string VarAlloc::ConsumeInst(IR::Inst& inst) {
    return Define(inst, inst.Type());
}
std::string VarAlloc::Consume(const IR::Value& value) {
    if (value.IsEmpty())
        return {};
    if (!value.IsImmediate())
        return ConsumeInst(*value.InstRecursive());
    switch (value.Type()) {
    case IR::Type::U1:
        return value.U1() ? "true" : "false";
    case IR::Type::U8:
        return fmt::format("{}u", value.U8());
    case IR::Type::U16:
        return fmt::format("{}u", value.U16());
    case IR::Type::U32:
        return fmt::format("{}u", value.U32());
    case IR::Type::U64:
        return fmt::format("(uint64_t({}u) | (uint64_t({}u) << 32))", u32(value.U64()),
                           u32(value.U64() >> 32));
    case IR::Type::F32:
        return FormatFloatImmediate(std::bit_cast<u32>(value.F32()));
    case IR::Type::F64: {
        const auto bits = std::bit_cast<u64>(value.F64());
        return fmt::format("asdouble(0x{:08x}u, 0x{:08x}u)", u32(bits), u32(bits >> 32));
    }
    default:
        throw NotImplementedException("HLSL immediate {}", value.Type());
    }
}
std::string VarAlloc::GetHlslType(IR::Type type) const {
    return GetHlslType(RegType(type));
}
namespace {
// Diagnostic: NXBOX_HLSL_NO_PRECISE=1 drops the qualifier from every stage, not only compute.
bool NoPrecise() {
    static const bool value = [] {
        const char* env = std::getenv("NXBOX_HLSL_NO_PRECISE");
        return env && env[0] == '1';
    }();
    return value;
}
} // namespace

std::string VarAlloc::GetHlslType(HlslVarType type) const {
    switch (type) {
    case HlslVarType::U1:
        return "bool";
    case HlslVarType::U32:
        return "uint";
    case HlslVarType::U64:
        return "uint64_t";
    case HlslVarType::F32:
        return "float";
    case HlslVarType::PrecF32:
        return drop_precise || NoPrecise() ? "float" : "precise float";
    case HlslVarType::F64:
        return "double";
    case HlslVarType::PrecF64:
        return drop_precise || NoPrecise() ? "double" : "precise double";
    case HlslVarType::F64x2:
        return "double2";
    case HlslVarType::F64x3:
        return "double3";
    case HlslVarType::F64x4:
        return "double4";
    case HlslVarType::F16x2:
    case HlslVarType::F32x2:
        return "float2";
    case HlslVarType::F32x3:
        return "float3";
    case HlslVarType::F32x4:
        return "float4";
    case HlslVarType::U32x2:
        return "uint2";
    case HlslVarType::U32x3:
        return "uint3";
    case HlslVarType::U32x4:
        return "uint4";
    default:
        throw NotImplementedException("HLSL variable type {}", static_cast<u32>(type));
    }
}
std::string VarAlloc::Declarations() const {
    std::string result;
    for (const auto& variable : variables)
        result += fmt::format("{} {} = 0;\n", GetHlslType(variable.type), variable.name);
    return result;
}
} // namespace Shader::Backend::HLSL
