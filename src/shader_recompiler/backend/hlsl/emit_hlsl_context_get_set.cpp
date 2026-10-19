// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/runtime_info.h"
namespace Shader::Backend::HLSL {
void EmitGetAttribute(EmitContext& ctx, IR::Inst& inst, IR::Attribute attr,
                      std::string_view vertex) {
    if (IR::IsGeneric(attr) && (ctx.stage == Stage::Fragment || ctx.stage == Stage::Geometry) &&
        !ctx.runtime_info.previous_stage_stores.Generic(IR::GenericAttributeIndex(attr),
                                                        IR::GenericAttributeElement(attr))) {
        ctx.AddF32("{} = {};", inst, IR::GenericAttributeElement(attr) == 3 ? "1.0f" : "0.0f");
        return;
    }
    // D3D12 rasterizes points at a fixed 1px size and has no point-size semantic.
    if (attr == IR::Attribute::PointSize) {
        ctx.AddF32("{} = 1.0f;", inst);
        return;
    }
    // D3D12 has no gl_PointCoord either, and the renderer never expands points into quads
    // (point_size != 1 is rejected by UnsupportedState). The single fragment of a 1px point sits
    // on the centre of its sprite, so (0.5, 0.5) is the coordinate it would read.
    if (attr == IR::Attribute::PointSpriteS || attr == IR::Attribute::PointSpriteT) {
        ctx.AddF32("{} = 0.5f;", inst);
        return;
    }
    auto value = ctx.Attribute(attr, false);
    if (ctx.stage == Stage::Geometry && value.starts_with("input."))
        value.replace(0, 6, fmt::format("inputs[{}].", vertex.empty() ? "0u" : vertex));
    if (IR::IsGeneric(attr) && (ctx.stage == Stage::VertexA || ctx.stage == Stage::VertexB)) {
        const auto type = ctx.runtime_info.generic_input_types[IR::GenericAttributeIndex(attr)];
        if (type == AttributeType::SignedInt || type == AttributeType::UnsignedInt)
            value = fmt::format("asfloat({})", value);
        else if (type == AttributeType::SignedScaled || type == AttributeType::UnsignedScaled)
            value = fmt::format("float({})", value);
        else if (type == AttributeType::SignedNorm10 || type == AttributeType::SignedInt10) {
            // D3D12 has no R10G10B10A2_SNORM/SINT; the IA fetches R10G10B10A2_UINT and the
            // shader sign-extends each component (10-bit xyz, 2-bit w) here.
            const u32 bits = IR::GenericAttributeElement(attr) == 3 ? 2 : 10;
            const auto wide = fmt::format("(int({} << {}u) >> {}u)", value, 32 - bits, 32 - bits);
            value = type == AttributeType::SignedInt10
                        ? fmt::format("asfloat({})", wide)
                        : fmt::format("max(float({}) * {}, -1.0f)", wide,
                                      bits == 2 ? "1.0f" : "(1.0f / 511.0f)");
        } else if (type == AttributeType::Disabled)
            value = IR::GenericAttributeElement(attr) == 3 ? "1.0f" : "0.0f";
    } else if (attr == IR::Attribute::FrontFace || attr == IR::Attribute::VertexId ||
               attr == IR::Attribute::InstanceId || attr == IR::Attribute::PrimitiveId ||
               attr == IR::Attribute::Layer || attr == IR::Attribute::BaseVertex ||
               attr == IR::Attribute::BaseInstance || attr == IR::Attribute::DrawID) {
        value = fmt::format("asfloat({})", value);
    }
    // Maxwell fragment PositionW is reciprocal clip W, matching GLSL gl_FragCoord.w.
    if (ctx.stage == Stage::Fragment && attr == IR::Attribute::PositionW)
        value = "(1.0f / input.position.w)";
    ctx.AddF32("{} = {};", inst, value);
}
void EmitGetAttributeU32(EmitContext& ctx, IR::Inst& inst, IR::Attribute attr,
                         std::string_view vertex) {
    auto value = ctx.Attribute(attr, false);
    if (ctx.stage == Stage::Geometry && value.starts_with("input."))
        value.replace(0, 6, fmt::format("inputs[{}].", vertex.empty() ? "0u" : vertex));
    ctx.AddU32("{} = {};", inst, value);
}
void EmitSetAttribute(EmitContext& ctx, IR::Attribute attr, std::string_view value,
                      std::string_view) {
    if (attr == IR::Attribute::PointSize)
        return; // ponytail: D3D12 points are always 1px; emulate with quad expansion if a game
                // needs it.
    if (attr == IR::Attribute::Layer || attr == IR::Attribute::ViewportIndex)
        ctx.Add("{} = asuint({});", ctx.Attribute(attr, true), value);
    else
        ctx.Add("{} = {};", ctx.Attribute(attr, true), value);
}
void EmitGetAttributeIndexed(EmitContext& ctx, IR::Inst& inst, std::string_view offset,
                             std::string_view) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::F32);
    ctx.Add("{} = 0.0f; switch ({} >> 2) {{", result, offset);
    for (u32 i = 0; i < 128; ++i) {
        const auto attr =
            static_cast<IR::Attribute>(static_cast<u32>(IR::Attribute::Generic0X) + i);
        if (ctx.info.loads[attr]) {
            const bool missing = ctx.stage == Stage::Fragment &&
                                 !ctx.runtime_info.previous_stage_stores.Generic(i / 4, i % 4);
            const auto value =
                missing ? (i % 4 == 3 ? "1.0f" : "0.0f") : ctx.Attribute(attr, false);
            ctx.Add("case {}u: {} = {}; break;", static_cast<u32>(attr), result, value);
        }
    }
    ctx.Add("}}");
}
void EmitSetFragColor(EmitContext& ctx, u32 index, u32 component, std::string_view value) {
    if (index == 0u && component == 3u && ctx.runtime_info.alpha_test_func)
        ctx.Add("nx_alpha = {};", value);
    if (!ctx.info.stores_frag_color.at(index))
        return;
    const auto type = ctx.runtime_info.color_output_types.at(index);
    const auto cast = type == AttributeType::SignedInt     ? "asint"
                      : type == AttributeType::UnsignedInt ? "asuint"
                                                           : "float";
    ctx.Add("output.color{}.{} = {}({});", index, "xyzw"[component], cast, value);
}
void EmitSetFragDepth(EmitContext& ctx, std::string_view value) {
    ctx.Add("output.depth = {};", value);
}
void EmitSetSampleMask(EmitContext& ctx, std::string_view value) {
    ctx.Add("output.coverage = {};", value);
}
void EmitLocalInvocationId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32x3("{} = local_id;", inst);
}
void EmitWorkgroupId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32x3("{} = group_id;", inst);
}
void EmitInvocationInfo(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = 0x00ff0000u;", inst);
}
void EmitSampleId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{} = input.sample_id;", inst);
}
void EmitYDirection(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddF32("{} = nx_y_direction;", inst);
}
void EmitResolutionDownFactor(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddF32("{} = nx_resolution_down_factor;", inst);
}
void EmitRenderArea(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddF32x4("{} = nx_render_area;", inst);
}
void EmitLoadLocal(EmitContext& ctx, IR::Inst& inst, std::string_view word_offset) {
    ctx.AddU32("{} = lmem[{}];", inst, word_offset);
}
void EmitWriteLocal(EmitContext& ctx, std::string_view word_offset, std::string_view value) {
    ctx.Add("lmem[{}] = {};", word_offset, value);
}
void EmitIsTextureScaled(EmitContext& ctx, IR::Inst& inst, const IR::Value& index) {
    ctx.AddU1("{} = (nx_texture_scaling_mask & (1u << {})) != 0u;", inst,
              ctx.var_alloc.Consume(index));
}
void EmitIsImageScaled(EmitContext& ctx, IR::Inst& inst, const IR::Value& index) {
    ctx.AddU1("{} = (nx_image_scaling_mask & (1u << {})) != 0u;", inst,
              ctx.var_alloc.Consume(index));
}
void EmitSetAttributeIndexed(EmitContext& ctx, std::string_view offset, std::string_view value,
                             std::string_view vertex) {
    ctx.Add("switch ({} >> 2) {{", offset);
    for (u32 i = 0; i < 128; ++i) {
        const auto attr =
            static_cast<IR::Attribute>(static_cast<u32>(IR::Attribute::Generic0X) + i);
        if (ctx.info.stores[attr])
            ctx.Add("case {}u: {} = {}; break;", static_cast<u32>(attr), ctx.Attribute(attr, true),
                    value);
    }
    ctx.Add("}}");
    if (ctx.stage == Stage::TessellationControl && !vertex.empty())
        throw NotImplementedException("HLSL hull indexed cross-control-point output");
}
void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    if (ctx.stage != Stage::Geometry)
        throw NotImplementedException("HLSL EmitVertex outside geometry stage");
    if (!stream.IsImmediate() || stream.U32() != 0u)
        throw NotImplementedException("HLSL geometry output stream requires stream-zero linkage");
    ctx.Add("{{ Output nx_vertex = output;");
    ctx.FinishOutput("nx_vertex");
    ctx.Add("nx_stream.Append(nx_vertex); }}");
}
void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    if (ctx.stage != Stage::Geometry)
        throw NotImplementedException("HLSL EndPrimitive outside geometry stage");
    if (!stream.IsImmediate() || stream.U32() != 0u)
        throw NotImplementedException("HLSL geometry restart requires stream-zero linkage");
    ctx.Add("nx_stream.RestartStrip();");
}
void EmitInvocationId(EmitContext& ctx, IR::Inst& inst) {
    if (ctx.stage != Stage::Geometry && ctx.stage != Stage::TessellationControl)
        throw NotImplementedException("HLSL invocation ID requires geometry or hull stage");
    ctx.AddU32("{} = nx_invocation_id;", inst);
}
} // namespace Shader::Backend::HLSL
