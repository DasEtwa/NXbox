// SPDX-License-Identifier: GPL-3.0-or-later
// Dispatch follows the GLSL backend's typed opcode emitters.
#include <algorithm>
#include <tuple>
#include <type_traits>
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
namespace Shader::Backend::HLSL {
namespace {
template <class Func>
struct FuncTraits {};

template <class ReturnType_, class... Args>
struct FuncTraits<ReturnType_ (*)(Args...)> {
    using ReturnType = ReturnType_;

    static constexpr size_t NUM_ARGS = sizeof...(Args);

    template <size_t I>
    using ArgType = std::tuple_element_t<I, std::tuple<Args...>>;
};

template <typename ArgType>
auto Arg(EmitContext& ctx, const IR::Value& arg) {
    if constexpr (std::is_same_v<ArgType, std::string_view>) {
        return ctx.var_alloc.Consume(arg);
    } else if constexpr (std::is_same_v<ArgType, const IR::Value&>) {
        return arg;
    } else if constexpr (std::is_same_v<ArgType, u32>) {
        return arg.U32();
    } else if constexpr (std::is_same_v<ArgType, IR::Attribute>) {
        return arg.Attribute();
    } else if constexpr (std::is_same_v<ArgType, IR::Patch>) {
        return arg.Patch();
    } else if constexpr (std::is_same_v<ArgType, IR::Pred>) {
        return arg.Pred();
    } else if constexpr (std::is_same_v<ArgType, IR::Reg>) {
        return arg.Reg();
    }
}

template <auto func, bool is_first_arg_inst, size_t... I>
void Invoke(EmitContext& ctx, IR::Inst* inst, std::index_sequence<I...>) {
    using Traits = FuncTraits<decltype(func)>;
    if constexpr (is_first_arg_inst) {
        func(ctx, *inst, Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
    } else {
        func(ctx, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
    }
}

template <auto func>
void Invoke(EmitContext& ctx, IR::Inst* inst) {
    using Traits = FuncTraits<decltype(func)>;
    static_assert(Traits::NUM_ARGS >= 1, "Insufficient arguments");
    if constexpr (Traits::NUM_ARGS == 1) {
        Invoke<func, false>(ctx, inst, std::make_index_sequence<0>{});
    } else {
        using FirstArgType = typename Traits::template ArgType<1>;
        static constexpr bool is_first_arg_inst = std::is_same_v<FirstArgType, IR::Inst&>;
        using Indices = std::make_index_sequence<Traits::NUM_ARGS - (is_first_arg_inst ? 2 : 1)>;
        Invoke<func, is_first_arg_inst>(ctx, inst, Indices{});
    }
}

void EmitInst(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.handled_pseudo.contains(inst))
        return;
    switch (inst->GetOpcode()) {
#define OPCODE(name, result_type, ...)                                                             \
    case IR::Opcode::name:                                                                         \
        return Invoke<&Emit##name>(ctx, inst);
#include "shader_recompiler/frontend/ir/opcodes.inc"
#undef OPCODE
    }
    throw LogicError("Invalid opcode {}", inst->GetOpcode());
}

// Snapshot all incoming values before writing any phi. This preserves parallel
// assignment when loop-carried phis swap values on a back edge.
void EmitPhiCopies(EmitContext& ctx, const IR::Program& program, IR::Block* predecessor) {
    std::vector<std::pair<std::string, std::string>> moves;
    for (auto* block : program.blocks) {
        for (auto& phi : block->Instructions()) {
            if (!IR::IsPhi(phi))
                break;
            for (size_t i = 0; i < phi.NumArgs(); ++i) {
                if (phi.PhiBlock(i) != predecessor)
                    continue;
                const auto destination = ctx.var_alloc.Define(phi, phi.Type());
                const auto source = ctx.var_alloc.Consume(phi.Arg(i));
                const auto temporary = "incoming_" + destination;
                moves.emplace_back(destination, temporary);
                ctx.Add("{} {} = {};", ctx.var_alloc.GetHlslType(phi.Type()), temporary, source);
            }
        }
    }
    for (const auto& [destination, temporary] : moves)
        ctx.Add("{} = {};", destination, temporary);
}
void EmitCode(EmitContext& ctx, const IR::Program& program) {
    size_t loop_index = 0;
    for (const auto& node : program.syntax_list) {
        using Node = IR::AbstractSyntaxNode;
        switch (node.type) {
        case Node::Type::Block:
            ctx.Add("{{");
            for (auto& inst : node.data.block->Instructions())
                EmitInst(ctx, &inst);
            EmitPhiCopies(ctx, program, node.data.block);
            ctx.Add("}}");
            break;
        case Node::Type::If:
            ctx.Add("if ({}) {{", ctx.var_alloc.Consume(node.data.if_node.cond));
            break;
        case Node::Type::EndIf:
            ctx.Add("}}");
            break;
        case Node::Type::Loop:
            ctx.Add("while (true) {{");
            break;
        case Node::Type::Repeat:
            ctx.Add("if (--loop{} < 0 || !({})) break; }}", loop_index++,
                    ctx.var_alloc.Consume(node.data.repeat.cond));
            break;
        case Node::Type::Break:
            ctx.Add("if ({}) break;", ctx.var_alloc.Consume(node.data.break_node.cond));
            break;
        case Node::Type::Return:
        case Node::Type::Unreachable:
            if (program.stage != Stage::Geometry)
                ctx.FinishOutput();
            ctx.Add("{}", ctx.ReturnStatement());
            break;
        }
    }
}
} // namespace
u32 RequiredShaderModel(const IR::Program& program) {
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            const auto name = IR::NameOf(inst.GetOpcode());
            if (inst.GetOpcode() == IR::Opcode::IsHelperInvocation ||
                inst.GetOpcode() == IR::Opcode::SharedAtomicExchange32x2 ||
                (name.find("Atomic") != name.npos && name.ends_with("64")))
                return 66u;
        }
    }
    return 60u;
}
std::string EmitHLSL(const Profile& profile, const RuntimeInfo& runtime_info, IR::Program& program,
                     Bindings& bindings) {
    // Do not publish partially advanced descriptor counters if emission fails.
    Bindings pending = bindings;
    EmitContext ctx{program, pending, profile, runtime_info};
    EmitCode(ctx, program);
    if (program.stage == Stage::Compute) {
        const auto& size = program.workgroup_size;
        if (!size[0] || !size[1] || !size[2] || size[0] > 1024 || size[1] > 1024 || size[2] > 64 ||
            u64(size[0]) * size[1] * size[2] > 1024)
            throw NotImplementedException("HLSL invalid compute workgroup size");
        ctx.header += fmt::format("[numthreads({}, {}, {})]\nvoid main(uint3 local_id : "
                                  "SV_GroupThreadID, uint3 group_id : SV_GroupID) {{\n",
                                  size[0], size[1], size[2]);
    } else if (program.stage == Stage::Geometry) {
        const auto topology = runtime_info.input_topology;
        const auto input_type = topology == InputTopology::Points           ? "point"
                                : topology == InputTopology::Lines          ? "line"
                                : topology == InputTopology::LinesAdjacency ? "lineadj"
                                : topology == InputTopology::Triangles      ? "triangle"
                                                                            : "triangleadj";
        const auto stream_type =
            program.output_topology == OutputTopology::PointList   ? "PointStream"
            : program.output_topology == OutputTopology::LineStrip ? "LineStream"
                                                                   : "TriangleStream";
        if (!program.output_vertices || program.output_vertices > 1024u ||
            program.invocations > 32u)
            throw NotImplementedException("HLSL invalid geometry output/invocation count");
        ctx.header += fmt::format(
            "[maxvertexcount({})]\n[instance({})]\nvoid main({} Input inputs[{}], "
            "uint nx_primitive_id : SV_PrimitiveID, uint nx_invocation_id : SV_GSInstanceID, "
            "inout {}<Output> nx_stream) {{\nInput input = inputs[0];\n"
            "Output output = (Output)0; output.position = float4(0, 0, 0, 1);\n",
            program.output_vertices, std::max(program.invocations, 1u), input_type,
            InputTopologyVertices::vertices(topology), stream_type);
    } else {
        if (runtime_info.force_early_z && program.stage == Stage::Fragment)
            ctx.header += "[earlydepthstencil]\n";
        ctx.header += (ctx.ReturnStatement() == "return;" ? "void" : "Output");
        ctx.header += " main(Input input) {\nOutput output = (Output)0;\n";
        if (program.stage != Stage::Fragment)
            ctx.header += "output.position = float4(0, 0, 0, 1);\n";
        for (size_t i = 0; i < 32; ++i)
            if (program.info.stores.Generic(i))
                ctx.header += fmt::format("output.attr{} = float4(0, 0, 0, 1);\n", i);
    }
    if (program.stage == Stage::Fragment && runtime_info.alpha_test_func)
        ctx.header += "float nx_alpha = 0.0f;\n";
    if (program.local_memory_size)
        ctx.header += fmt::format("uint lmem[{}];\n", (program.local_memory_size + 3) / 4);
    ctx.header += ctx.var_alloc.Declarations();
    if (ctx.uses_cc_carry)
        ctx.header += "uint carry;\n";
    ctx.header += "bool shfl_in_bounds = false;\n";
    if (ctx.uses_register_state)
        ctx.header +=
            "uint nx_regs[255] = (uint[255])0; bool nx_preds[7] = (bool[7])0; "
            "uint nx_branch = 0u; bool nx_z = false, nx_s = false, nx_c = false, nx_o = false;\n";
    for (const auto index : ctx.goto_variables)
        ctx.header += fmt::format("bool nx_goto_{} = false;\n", index);
    size_t loop_index = 0;
    for (const auto& node : program.syntax_list)
        if (node.type == IR::AbstractSyntaxNode::Type::Repeat)
            ctx.header += fmt::format("int loop{} = 0x2000;\n", loop_index++);
    if (program.stage != Stage::Geometry)
        ctx.FinishOutput();
    ctx.Add("{}", ctx.ReturnStatement());
    bindings = pending;
    return ctx.header + ctx.code + "}\n";
}
} // namespace Shader::Backend::HLSL
