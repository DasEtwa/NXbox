// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/runtime_info.h"
namespace Shader::Backend::HLSL {
void EmitGetAttribute(EmitContext& ctx, IR::Inst& inst, IR::Attribute attr, std::string_view) {
    if (IR::IsGeneric(attr) && ctx.stage == Stage::Fragment &&
        !ctx.runtime_info.previous_stage_stores.Generic(IR::GenericAttributeIndex(attr),
                                                        IR::GenericAttributeElement(attr))) {
        ctx.AddF32("{} = {};", inst, IR::GenericAttributeElement(attr) == 3 ? "1.0f" : "0.0f");
        return;
    }
    auto value = ctx.Attribute(attr, false);
    if (IR::IsGeneric(attr) && ctx.stage != Stage::Fragment) {
        const auto type = ctx.runtime_info.generic_input_types[IR::GenericAttributeIndex(attr)];
        if (type == AttributeType::SignedInt || type == AttributeType::UnsignedInt)
            value = fmt::format("asfloat({})", value);
        else if (type == AttributeType::Disabled)
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
void EmitGetAttributeU32(EmitContext& ctx, IR::Inst& inst, IR::Attribute attr, std::string_view) {
    ctx.AddU32("{} = {};", inst, ctx.Attribute(attr, false));
}
void EmitSetAttribute(EmitContext& ctx, IR::Attribute attr, std::string_view value,
                      std::string_view) {
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
} // namespace Shader::Backend::HLSL
