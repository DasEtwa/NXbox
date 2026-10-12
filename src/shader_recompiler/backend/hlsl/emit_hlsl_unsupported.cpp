// SPDX-License-Identifier: GPL-3.0-or-later
// Fail closed for unlowered frontend operations and unsupported SM 6.0 features.
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/exception.h"
namespace Shader::Backend::HLSL {
void EmitGetPatch(EmitContext&, IR::Inst&, IR::Patch) {
    throw NotImplementedException("HLSL GetPatch requires hull/domain patch linkage");
}
void EmitSetPatch(EmitContext&, IR::Patch, std::string_view) {
    throw NotImplementedException("HLSL SetPatch requires hull/domain patch linkage");
}
void EmitIsHelperInvocation(EmitContext& ctx, IR::Inst& inst) {
    // IsHelperLane is an SM 6.6 intrinsic; callers must select RequiredShaderModel.
    ctx.AddU1("{} = {};", inst, ctx.stage == Stage::Fragment ? "IsHelperLane()" : "false");
}
void EmitSR_WScaleFactorXY(EmitContext&, IR::Inst&) {
    throw NotImplementedException("HLSL SR_WScaleFactorXY has no defined Maxwell W-scale register "
                                  "mapping in the reference backends");
}
void EmitSR_WScaleFactorZ(EmitContext&, IR::Inst&) {
    throw NotImplementedException("HLSL SR_WScaleFactorZ has no defined Maxwell W-scale register "
                                  "mapping in the reference backends");
}
void EmitGlobalAtomicInc32x2(EmitContext&) {
    throw NotImplementedException(
        "HLSL GlobalAtomicInc32x2 requires a paired wrapping atomic implementation");
}
void EmitGlobalAtomicDec32x2(EmitContext&) {
    throw NotImplementedException(
        "HLSL GlobalAtomicDec32x2 requires a paired wrapping atomic implementation");
}
void EmitBindlessImageSampleImplicitLod(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageSampleImplicitLod must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageSampleExplicitLod(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageSampleExplicitLod must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageSampleDrefImplicitLod(EmitContext&) {
    throw NotImplementedException("HLSL BindlessImageSampleDrefImplicitLod must be resolved by "
                                  "TexturePass before HLSL emission");
}
void EmitBindlessImageSampleDrefExplicitLod(EmitContext&) {
    throw NotImplementedException("HLSL BindlessImageSampleDrefExplicitLod must be resolved by "
                                  "TexturePass before HLSL emission");
}
void EmitBindlessImageGather(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageGather must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageGatherDref(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageGatherDref must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageFetch(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageFetch must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageQueryDimensions(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageQueryDimensions must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageQueryLod(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageQueryLod must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageGradient(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageGradient must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageRead(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageRead must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageWrite(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageWrite must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageSampleImplicitLod(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageSampleImplicitLod must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageSampleExplicitLod(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageSampleExplicitLod must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageSampleDrefImplicitLod(EmitContext&) {
    throw NotImplementedException("HLSL BoundImageSampleDrefImplicitLod must be resolved by "
                                  "TexturePass before HLSL emission");
}
void EmitBoundImageSampleDrefExplicitLod(EmitContext&) {
    throw NotImplementedException("HLSL BoundImageSampleDrefExplicitLod must be resolved by "
                                  "TexturePass before HLSL emission");
}
void EmitBoundImageGather(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageGather must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageGatherDref(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageGatherDref must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageFetch(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageFetch must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageQueryDimensions(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageQueryDimensions must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageQueryLod(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageQueryLod must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageGradient(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageGradient must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageRead(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageRead must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageWrite(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageWrite must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicIAdd32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicIAdd32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicSMin32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicSMin32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicUMin32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicUMin32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicSMax32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicSMax32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicUMax32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicUMax32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicInc32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicInc32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicDec32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicDec32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicAnd32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicAnd32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicOr32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicOr32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicXor32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicXor32 must be resolved by TexturePass before HLSL emission");
}
void EmitBindlessImageAtomicExchange32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BindlessImageAtomicExchange32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicIAdd32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicIAdd32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicSMin32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicSMin32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicUMin32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicUMin32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicSMax32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicSMax32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicUMax32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicUMax32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicInc32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicInc32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicDec32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicDec32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicAnd32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicAnd32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicOr32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicOr32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicXor32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicXor32 must be resolved by TexturePass before HLSL emission");
}
void EmitBoundImageAtomicExchange32(EmitContext&) {
    throw NotImplementedException(
        "HLSL BoundImageAtomicExchange32 must be resolved by TexturePass before HLSL emission");
}
} // namespace Shader::Backend::HLSL
