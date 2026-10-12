// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"

#include "shader_recompiler/frontend/ir/modifiers.h"

using namespace Shader;
using Shader::Backend::Bindings;
namespace {
void Require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
void Contains(const std::string& text, std::string_view token) {
    Require(text.find(token) != text.npos, token);
}
struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Fixture(Stage stage) {
        program.stage = stage;
        program.blocks.push_back(&block);
        program.syntax_list.push_back(
            {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
        program.workgroup_size = {8, 4, 1};
    }
    IR::Value Add(IR::Opcode opcode, std::initializer_list<IR::Value> args = {}, u32 flags = 0) {
        auto it = block.PrependNewInst(block.end(), opcode, args, flags);
        return IR::Value{&*it};
    }
    std::string Emit(Bindings& bindings) {
        return Backend::HLSL::EmitHLSL({}, {}, program, bindings);
    }
};
void Vertex() {
    Fixture f{Stage::VertexB};
    f.program.info.loads.Set(IR::Attribute::Generic0X);
    f.program.info.stores.Set(IR::Attribute::PositionX);
    const auto input =
        f.Add(IR::Opcode::GetAttribute, {IR::Value{IR::Attribute::Generic0X}, IR::Value{0u}});
    const auto sum = f.Add(IR::Opcode::FPAdd32, {input, IR::Value{2.0f}});
    f.Add(IR::Opcode::SetAttribute, {IR::Value{IR::Attribute::PositionX}, sum, IR::Value{0u}});
    Bindings bindings;
    const auto text = f.Emit(bindings);
    Contains(text, "Output main(Input input)");
    Contains(text, "SV_Position");
    Contains(text, "TEXCOORD0");
    Contains(text, "output.position.x =");
    const auto second = f.Emit(bindings);
    Require(text == second, "Emission must preserve the IR");
    std::ofstream("hlsl_vertex.hlsl") << text;
}
void Fragment() {
    Fixture f{Stage::Fragment};
    f.program.info.stores_frag_color[0] = true;
    f.program.info.stores_frag_depth = true;
    f.program.info.texture_descriptors.push_back({.type = TextureType::Color2D, .count = 1});
    const auto coords =
        f.Add(IR::Opcode::CompositeConstructF32x2, {IR::Value{0.5f}, IR::Value{0.25f}});
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    const auto sample =
        f.Add(IR::Opcode::ImageSampleImplicitLod, {IR::Value{0u}, coords, {}, {}}, flags.raw);
    const auto red = f.Add(IR::Opcode::CompositeExtractF32x4, {sample, IR::Value{0u}});
    f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, red});
    f.Add(IR::Opcode::SetFragDepth, {IR::Value{0.75f}});
    Bindings bindings;
    const auto text = f.Emit(bindings);
    Contains(text, "Texture2D<float4> tex0 : register(t0, space0)");
    Contains(text, "SamplerState samp0 : register(s0, space0)");
    Contains(text, ".Sample(samp0,");
    Contains(text, "SV_Target0");
    Contains(text, "SV_Depth");
    Require(bindings.texture == 1, "Texture counter");
    std::ofstream("hlsl_fragment.hlsl") << text;
    // Attachment specialization clears export metadata but keeps the original IR.
    // Stores into unbound targets must disappear from both declarations and body.
    f.Add(IR::Opcode::SetFragColor, {IR::Value{3u}, IR::Value{0u}, red});
    Bindings masked_bindings;
    const auto masked = f.Emit(masked_bindings);
    Require(masked.find("SV_Target3") == masked.npos, "Unbound target signature");
    Require(masked.find("output.color3") == masked.npos, "Unbound target store");
    f.program.info.stores_frag_color[0] = false;
    Bindings depth_bindings;
    const auto depth_only = f.Emit(depth_bindings);
    Require(depth_only.find("SV_Target") == depth_only.npos, "Depth-only color signature");
    Require(depth_only.find("output.color0") == depth_only.npos, "Depth-only color store");
    Contains(depth_only, "SV_Depth");
}
void Compute() {
    Fixture f{Stage::Compute};
    f.program.shared_memory_size = 128;
    f.program.info.constant_buffer_descriptors.push_back({.index = 3, .count = 1});
    f.program.info.storage_buffers_descriptors.push_back({.count = 1, .is_written = true});
    const auto value = f.Add(IR::Opcode::GetCbufU32, {IR::Value{3u}, IR::Value{16u}});
    const auto old = f.Add(IR::Opcode::StorageAtomicIAdd32, {IR::Value{0u}, IR::Value{0u}, value});
    f.Add(IR::Opcode::WriteSharedU32, {IR::Value{4u}, old});
    f.Add(IR::Opcode::Barrier);
    f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{4u}, old});
    Bindings bindings;
    const auto text = f.Emit(bindings);
    Contains(text, "register(b0, space1)");
    Contains(text, "nx_cbuf3 : register(b0, space0)");
    Contains(text, "RWByteAddressBuffer ssbo0 : register(u0, space0)");
    Contains(text, ".InterlockedAdd(");
    Contains(text, "groupshared uint smem[32]");
    Contains(text, "GroupMemoryBarrierWithGroupSync");
    Contains(text, "[numthreads(8, 4, 1)]");
    Require(bindings.uniform_buffer == 1 && bindings.storage_buffer == 1, "Buffer counters");
    std::ofstream("hlsl_compute.hlsl") << text;
}
void ControlFlowAndWaves() {
    Fixture f{Stage::Compute};
    f.program.info.uses_subgroup_vote = true;
    f.program.info.uses_subgroup_shuffles = true;
    f.program.info.storage_buffers_descriptors.push_back({.count = 1, .is_written = true});
    IR::Block header{f.pool};
    IR::Block body{f.pool};
    f.program.blocks.push_back(&header);
    f.program.blocks.push_back(&body);
    auto first_it =
        header.PrependNewInst(header.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U32));
    auto second_it =
        header.PrependNewInst(header.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U32));
    first_it->AddPhiOperand(&f.block, IR::Value{11u});
    second_it->AddPhiOperand(&f.block, IR::Value{22u});
    first_it->AddPhiOperand(&body, IR::Value{&*second_it});
    second_it->AddPhiOperand(&body, IR::Value{&*first_it});
    const auto condition = f.Add(IR::Opcode::ConditionRef, {IR::Value{true}});
    f.Add(IR::Opcode::SubgroupBallot, {condition});
    f.Add(IR::Opcode::VoteAll, {condition});
    const auto shuffle = f.Add(IR::Opcode::ShuffleIndex,
                               {IR::Value{7u}, IR::Value{1u}, IR::Value{31u}, IR::Value{0u}});
    const auto flag = f.Add(IR::Opcode::GetInBoundsFromOp, {shuffle});
    f.Add(IR::Opcode::LogicalNot, {flag});
    body.AppendNewInst(IR::Opcode::WriteStorage32,
                       {IR::Value{0u}, IR::Value{0u}, IR::Value{&*first_it}});
    using Node = IR::AbstractSyntaxNode;
    f.program.syntax_list.push_back(
        {.data = {.loop = {.body = &header}}, .type = Node::Type::Loop});
    f.program.syntax_list.push_back({.data = {.block = &header}, .type = Node::Type::Block});
    f.program.syntax_list.push_back(
        {.data = {.if_node = {.cond = IR::U1{condition}}}, .type = Node::Type::If});
    f.program.syntax_list.push_back({.data = {.block = &body}, .type = Node::Type::Block});
    f.program.syntax_list.push_back({.type = Node::Type::EndIf});
    f.program.syntax_list.push_back(
        {.data = {.repeat = {.cond = IR::U1{IR::Value{false}}}}, .type = Node::Type::Repeat});
    Bindings bindings;
    const auto text = f.Emit(bindings);
    Contains(text, "incoming_v");
    Contains(text, "while (true)");
    Contains(text, "int loop0 = 0x2000");
    Contains(text, "nx_shuffle(");
    Contains(text, "nx_ballot(");
    Contains(text, "WaveReadLaneAt");
    // Check parallel phi assignments: every snapshot precedes the first write.
    const auto start = text.find("uint incoming_");
    const auto next = text.find("uint incoming_", start + 1);
    const auto write = text.find(" = incoming_", start);
    Require(start < next && next < write, "Phi snapshots must precede phi writes");
    Bindings repeated;
    Require(text == f.Emit(repeated), "Wave/pseudo/phi emission modified IR");
    std::ofstream("hlsl_waves.hlsl") << text;
}
void ResourceOrderAndCompare() {
    Fixture f{Stage::Fragment};
    f.program.info.stores_frag_color[0] = true;
    f.program.info.texture_descriptors.push_back(
        {.type = TextureType::Color2D, .is_depth = true, .count = 1});
    f.program.info.texture_buffer_descriptors.push_back({.count = 1});
    f.program.info.storage_buffers_descriptors.push_back({.count = 1});
    f.program.info.image_descriptors.push_back({.type = TextureType::Color2D,
                                                .format = ImageFormat::R32_UINT,
                                                .is_written = true,
                                                .is_integer = true,
                                                .count = 1});
    const auto coords =
        f.Add(IR::Opcode::CompositeConstructF32x2, {IR::Value{0.5f}, IR::Value{0.25f}});
    IR::TextureInstInfo depth{};
    depth.type.Assign(TextureType::Color2D);
    depth.is_depth.Assign(1);
    const auto compare =
        f.Add(IR::Opcode::ImageSampleDrefExplicitLod,
              {IR::Value{0u}, coords, IR::Value{0.5f}, IR::Value{0.0f}, {}}, depth.raw);
    f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, compare});
    f.Add(IR::Opcode::ImageQueryDimensions, {IR::Value{0u}, IR::Value{0u}, IR::Value{false}},
          depth.raw);
    IR::TextureInstInfo gather{};
    gather.type.Assign(TextureType::Color2D);
    gather.is_depth.Assign(1);
    f.Add(IR::Opcode::ImageGatherDref, {IR::Value{0u}, coords, {}, {}, IR::Value{0.5f}},
          gather.raw);
    const auto int_coords =
        f.Add(IR::Opcode::CompositeConstructU32x2, {IR::Value{1u}, IR::Value{2u}});
    IR::TextureInstInfo image{};
    image.type.Assign(TextureType::Color2D);
    image.image_format.Assign(ImageFormat::R32_UINT);
    const auto value = f.Add(IR::Opcode::CompositeConstructU32x4,
                             {IR::Value{4u}, IR::Value{0u}, IR::Value{0u}, IR::Value{1u}});
    f.Add(IR::Opcode::ImageWrite, {IR::Value{0u}, int_coords, value}, image.raw);
    f.Add(IR::Opcode::ImageRead, {IR::Value{0u}, int_coords}, image.raw);
    f.Add(IR::Opcode::ImageAtomicIAdd32, {IR::Value{0u}, int_coords, IR::Value{3u}}, image.raw);
    Bindings bindings{.uniform_buffer = 2, .storage_buffer = 3, .texture = 4, .image = 2};
    const auto text = f.Emit(bindings);
    Contains(text, "tex4 : register(t4, space0)");
    Contains(text, "tex5 : register(t5, space0)");
    Contains(text, "ssbo0 : register(u5, space0)");
    Contains(text, "img6 : register(u6, space0)");
    Contains(text, "SamplerComparisonState samp4");
    Contains(text, ".SampleCmpLevelZero(");
    Contains(text, ".GatherCmp(");
    Contains(text, ".GetDimensions(");
    Contains(text, "InterlockedAdd(img6[");
    Require(bindings.texture == 6 && bindings.storage_buffer == 4 && bindings.image == 3,
            "Shared UAV/SRV counters");
    std::ofstream("hlsl_resources.hlsl") << text;
}
void ArithmeticAndPseudoResults() {
    Fixture f{Stage::Compute};
    f.program.info.uses_int64 = true;
    f.program.info.uses_fp64 = true;
    f.program.info.storage_buffers_descriptors.push_back({.count = 1, .is_written = true});
    const auto sum = f.Add(IR::Opcode::IAdd32, {IR::Value{0xffffffffu}, IR::Value{1u}});
    const auto carry = f.Add(IR::Opcode::GetCarryFromOp, {sum});
    f.Add(IR::Opcode::GetZeroFromOp, {sum});
    f.Add(IR::Opcode::GetSignFromOp, {sum});
    f.Add(IR::Opcode::GetOverflowFromOp, {sum});
    const auto select = f.Add(IR::Opcode::SelectU32, {carry, IR::Value{1u}, IR::Value{0u}});
    f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{0u}, select});
    const auto wide = f.Add(IR::Opcode::IAdd64, {IR::Value{u64{0x100000001}}, IR::Value{u64{2}}});
    f.Add(IR::Opcode::IAbs64, {wide});
    const auto pair = f.Add(IR::Opcode::UnpackUint2x32, {wide});
    f.Add(IR::Opcode::WriteStorage64, {IR::Value{0u}, IR::Value{8u}, pair});
    const auto rounded = f.Add(IR::Opcode::FPRoundEven64, {IR::Value{2.5}});
    const auto narrowed = f.Add(IR::Opcode::ConvertF32F64, {rounded});
    const auto equal = f.Add(IR::Opcode::FPOrdEqual32, {narrowed, IR::Value{2.0f}});
    f.Add(IR::Opcode::SelectU32, {equal, IR::Value{3u}, IR::Value{4u}});
    Bindings bindings;
    const auto text = f.Emit(bindings);
    Contains(text, "uaddCarry(");
    Contains(text, "carry!=0");
    Contains(text, "nx_abs_int64(");
    Contains(text, "(uint64_t(1u) | (uint64_t(1u) << 32))");
    Contains(text, "nx_round64(");
    Contains(text, "&&!isnan(");
    Bindings repeated;
    Require(text == f.Emit(repeated), "Arithmetic pseudo-result emission modified IR");
    std::ofstream("hlsl_arithmetic.hlsl") << text;
}
void ChainedResourceBindings() {
    Fixture vertex{Stage::VertexB};
    vertex.program.info.texture_descriptors.push_back({.type = TextureType::Color2D, .count = 1});
    vertex.program.info.texture_buffer_descriptors.push_back({.count = 1});
    vertex.program.info.storage_buffers_descriptors.push_back({.count = 1});
    vertex.program.info.image_descriptors.push_back(
        {.type = TextureType::Color2D, .format = ImageFormat::R32_UINT, .count = 1});
    vertex.program.info.image_buffer_descriptors.push_back(
        {.format = ImageFormat::R32_UINT, .count = 1});
    Bindings bindings;
    const auto first = vertex.Emit(bindings);
    Contains(first, "samp0 : register(s0, space0)");
    Contains(first, "ssbo0 : register(u0, space0)");
    Contains(first, "img1 : register(u1, space0)");
    Contains(first, "img2 : register(u2, space0)");
    Fixture fragment{Stage::Fragment};
    fragment.program.info.texture_descriptors.push_back({.type = TextureType::Color2D, .count = 1});
    fragment.program.info.storage_buffers_descriptors.push_back({.count = 1});
    const auto second = fragment.Emit(bindings);
    Contains(second, "tex2 : register(t2, space0)");
    Contains(second, "samp2 : register(s1, space0)");
    Contains(second, "ssbo0 : register(u3, space0)");
    Require(bindings.texture == 3 && bindings.hlsl_sampler == 2 &&
                bindings.storage_buffer + bindings.image == 4,
            "Stage append order must match native descriptor tables");
}
void UnsupportedRollback() {
    Fixture f{Stage::Compute};
    f.program.info.constant_buffer_descriptors.push_back({.index = 0, .count = 1});
    f.Add(IR::Opcode::IsHelperInvocation);
    Bindings bindings{.uniform_buffer = 7};
    try {
        f.Emit(bindings);
        throw std::runtime_error("Unsupported opcode silently accepted");
    } catch (const NotImplementedException& e) {
        Contains(e.what(), "EmitIsHelperInvocation");
    }
    Require(bindings.uniform_buffer == 7, "Failure advanced bindings");
    f.program.stage = Stage::Geometry;
    try {
        f.Emit(bindings);
        throw std::runtime_error("Geometry silently accepted");
    } catch (const NotImplementedException&) {
    }
}
} // namespace
int main() {
    try {
        Vertex();
        Fragment();
        Compute();
        ControlFlowAndWaves();
        ResourceOrderAndCompare();
        ArithmeticAndPseudoResults();
        ChainedResourceBindings();
        UnsupportedRollback();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
