// SPDX-License-Identifier: GPL-3.0-or-later
// Fail closed for unlowered frontend operations and unsupported SM 6.0 features.
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/exception.h"
namespace Shader::Backend::HLSL {
void EmitEmitVertex(EmitContext&, const IR::Value&) {
    NotImplemented();
}
void EmitEndPrimitive(EmitContext&, const IR::Value&) {
    NotImplemented();
}
void EmitSetAttributeIndexed(EmitContext&, std::string_view, std::string_view, std::string_view) {
    NotImplemented();
}
void EmitGetPatch(EmitContext&, IR::Inst&, IR::Patch) {
    NotImplemented();
}
void EmitSetPatch(EmitContext&, IR::Patch, std::string_view) {
    NotImplemented();
}
void EmitInvocationId(EmitContext&, IR::Inst&) {
    NotImplemented();
}
void EmitIsHelperInvocation(EmitContext&, IR::Inst&) {
    NotImplemented();
}
void EmitSR_WScaleFactorXY(EmitContext&, IR::Inst&) {
    NotImplemented();
}
void EmitSR_WScaleFactorZ(EmitContext&, IR::Inst&) {
    NotImplemented();
}
void EmitLoadGlobalU8(EmitContext&) {
    NotImplemented();
}
void EmitLoadGlobalS8(EmitContext&) {
    NotImplemented();
}
void EmitLoadGlobalU16(EmitContext&) {
    NotImplemented();
}
void EmitLoadGlobalS16(EmitContext&) {
    NotImplemented();
}
void EmitWriteGlobalU8(EmitContext&) {
    NotImplemented();
}
void EmitWriteGlobalS8(EmitContext&) {
    NotImplemented();
}
void EmitWriteGlobalU16(EmitContext&) {
    NotImplemented();
}
void EmitWriteGlobalS16(EmitContext&) {
    NotImplemented();
}
void EmitSharedAtomicExchange64(EmitContext&, IR::Inst&, std::string_view, std::string_view) {
    NotImplemented();
}
void EmitSharedAtomicExchange32x2(EmitContext&, IR::Inst&, std::string_view, std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicIAdd64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                             std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicSMin64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                             std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicUMin64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                             std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicSMax64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                             std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicUMax64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                             std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicAnd64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                            std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicOr64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                           std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicXor64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                            std::string_view) {
    NotImplemented();
}
void EmitStorageAtomicExchange64(EmitContext&, IR::Inst&, const IR::Value&, const IR::Value&,
                                 std::string_view) {
    NotImplemented();
}
void EmitGlobalAtomicIAdd32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicSMin32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicUMin32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicSMax32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicUMax32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicInc32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicDec32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicAnd32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicOr32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicXor32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicExchange32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicIAdd64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicSMin64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicUMin64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicSMax64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicUMax64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicInc64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicDec64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicAnd64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicOr64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicXor64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicExchange64(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicIAdd32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicSMin32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicUMin32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicSMax32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicUMax32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicInc32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicDec32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicAnd32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicOr32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicXor32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicExchange32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicAddF32(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicAddF16x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicAddF32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicMinF16x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicMinF32x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicMaxF16x2(EmitContext&) {
    NotImplemented();
}
void EmitGlobalAtomicMaxF32x2(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageSampleImplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageSampleExplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageSampleDrefImplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageSampleDrefExplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageGather(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageGatherDref(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageFetch(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageQueryDimensions(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageQueryLod(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageGradient(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageRead(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageWrite(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageSampleImplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageSampleExplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageSampleDrefImplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageSampleDrefExplicitLod(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageGather(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageGatherDref(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageFetch(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageQueryDimensions(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageQueryLod(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageGradient(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageRead(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageWrite(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicIAdd32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicSMin32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicUMin32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicSMax32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicUMax32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicInc32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicDec32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicAnd32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicOr32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicXor32(EmitContext&) {
    NotImplemented();
}
void EmitBindlessImageAtomicExchange32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicIAdd32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicSMin32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicUMin32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicSMax32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicUMax32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicInc32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicDec32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicAnd32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicOr32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicXor32(EmitContext&) {
    NotImplemented();
}
void EmitBoundImageAtomicExchange32(EmitContext&) {
    NotImplemented();
}
} // namespace Shader::Backend::HLSL
