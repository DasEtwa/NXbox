// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <bit>
#include <cstdlib>
#include <fmt/format.h>
#include "shader_recompiler/backend/hlsl/hlsl_source.h"
#include "shader_recompiler/backend/hlsl/var_alloc.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
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
void VarAlloc::PrepareUses(const IR::Program& program) {
    single_use_blocks.clear();
    const auto record = [&](const IR::Value& value, const IR::Block* block) {
        if (value.IsEmpty() || value.IsImmediate())
            return;
        const auto* producer = value.InstRecursive();
        const auto [found, inserted] = single_use_blocks.emplace(producer, block);
        if (!inserted)
            found->second = nullptr;
    };
    // Identity aliases can have several downstream readers despite UseCount()==1
    // on their producer. Count actual recursive consumers across the entire program.
    for (const auto* block : program.blocks)
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Identity ||
                inst.GetOpcode() == IR::Opcode::Reference)
                continue;
            // Retain texture-expression boundaries: glslang's propagation of
            // NoContraction through sampling arguments depends on these assignments.
            const bool image = IR::NameOf(inst.GetOpcode()).find("Image") != std::string_view::npos;
            for (size_t index = 0; index < inst.NumArgs(); ++index)
                record(inst.Arg(index), image ? nullptr : block);
        }
    using Node = IR::AbstractSyntaxNode;
    for (const auto& node : program.syntax_list)
        switch (node.type) {
        case Node::Type::If:
            record(node.data.if_node.cond, nullptr);
            break;
        case Node::Type::Repeat:
            record(node.data.repeat.cond, nullptr);
            break;
        case Node::Type::Break:
            record(node.data.break_node.cond, nullptr);
            break;
        default:
            break;
        }
}
void VarAlloc::InlineBlock(std::string& code, size_t begin, const IR::Block& block) {
    std::vector<InlineCandidate> candidates;
    for (const auto& inst : block.Instructions()) {
        const auto found = names.find(const_cast<IR::Inst*>(&inst));
        const auto use = single_use_blocks.find(&inst);
        if (found == names.end() || use == single_use_blocks.end() || use->second != &block ||
            inst.MayHaveSideEffects() || inst.HasAssociatedPseudoOperation() ||
            inst.IsPseudoInstruction() || IR::IsPhi(inst))
            continue;
        const auto& variable = variables[found->second];
        if (variable.type == HlslVarType::PrecF32 || variable.type == HlslVarType::PrecF64)
            continue;
        // Fragment PositionW contains a reciprocal. Keeping its assignment
        // preserves glslang's sharing and transitive NoContraction decoration.
        if ((inst.GetOpcode() == IR::Opcode::GetAttribute ||
             inst.GetOpcode() == IR::Opcode::GetAttributeU32) &&
            inst.Arg(0).Attribute() == IR::Attribute::PositionW)
            continue;
        const auto opcode = IR::NameOf(inst.GetOpcode());
        // Reads of mutable memory/state and lane operations must keep their position.
        // Keep texture precision boundaries and immutable-read conversion rules intact.
        if (opcode.find("Image") != opcode.npos || opcode.find("Load") != opcode.npos || opcode.find("Read") != opcode.npos ||
            opcode.find("Atomic") != opcode.npos || opcode.find("Shuffle") != opcode.npos ||
            opcode.find("Subgroup") != opcode.npos || opcode.find("Vote") != opcode.npos ||
            opcode.find("Quad") != opcode.npos || opcode.find("Swizzle") != opcode.npos ||
            opcode.starts_with("DPd") || opcode == "IsHelperInvocation" ||
            (opcode.starts_with("Get") && !opcode.starts_with("GetCbuf") &&
             opcode != "GetAttribute" && opcode != "GetAttributeU32"))
            continue;
        // These emitters already produce the exact IR result type. Other forms
        // retain an explicit cast to preserve the assignment's implicit conversion.
        const bool exact_type = opcode.starts_with("FP") || opcode.starts_with("Composite") ||
                                opcode.starts_with("BitCast") || opcode == "IAdd32" ||
                                opcode == "IMul32" || opcode.starts_with("Bitwise") ||
                                opcode == "GetCbufU32" || opcode == "GetCbufF32";
        candidates.push_back({variable.name, GetHlslType(variable.type), exact_type});
    }
    const auto inlined = InlineSingleUseBlock(std::string_view{code}.substr(begin), candidates);
    code.replace(begin, code.size() - begin, inlined.source);
    for (const auto& name : inlined.eliminated) {
        size_t index{};
        const auto parsed = std::from_chars(name.data() + 1, name.data() + name.size(), index);
        if (parsed.ec == std::errc{} && index < variables.size())
            variables[index].eliminated = true;
    }
}
std::string VarAlloc::Declarations() const {
    std::string result;
    for (const auto& variable : variables)
        if (!variable.eliminated)
            result += fmt::format("{} {} = 0;\n", GetHlslType(variable.type), variable.name);
    return result;
}
} // namespace Shader::Backend::HLSL
