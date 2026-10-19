// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/runtime_info.h"
namespace Shader::Backend::HLSL {
void EmitGetCbufU8(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                   const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    ctx.AddU32("{} = bitfieldExtract(uint({}), int(({} & 3u) * 8u), 8);", inst,
               ctx.CbufferLoad(binding, offset), o);
}

void EmitGetCbufS8(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                   const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    ctx.AddU32("{} = bitfieldExtract(int({}), int(({} & 3u) * 8u), 8);", inst,
               ctx.CbufferLoad(binding, offset), o);
}

void EmitGetCbufU16(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    ctx.AddU32("{} = bitfieldExtract(uint({}), int(({} & 2u) * 8u), 16);", inst,
               ctx.CbufferLoad(binding, offset), o);
}

void EmitGetCbufS16(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    ctx.AddU32("{} = bitfieldExtract(int({}), int(({} & 2u) * 8u), 16);", inst,
               ctx.CbufferLoad(binding, offset), o);
}

void EmitGetCbufU32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    ctx.AddU32("{} = {};", inst, ctx.CbufferLoad(binding, offset));
}

void EmitGetCbufF32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    ctx.AddF32("{} = asfloat({});", inst, ctx.CbufferLoad(binding, offset));
}

void EmitGetCbufU32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                      const IR::Value& offset) {
    ctx.AddU32x2("{} = uint2({}, {});", inst, ctx.CbufferLoad(binding, offset),
                 ctx.CbufferLoad(binding, offset, 4));
}

void EmitLoadSharedU8(EmitContext& ctx, IR::Inst& inst, std::string_view offset) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = bitfieldExtract(uint(nx_shared_load32({})), int(({} & 3u) * 8u), 8);",
                   inst, offset, offset);
        return;
    }

    const auto o = std::string(offset);
    ctx.AddU32("{} = bitfieldExtract(uint(smem[{} >> 2]), int(({} & 3u) * 8u), 8);", inst, o, o);
}

void EmitLoadSharedS8(EmitContext& ctx, IR::Inst& inst, std::string_view offset) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = bitfieldExtract(int(nx_shared_load32({})), int(({} & 3u) * 8u), 8);", inst,
                   offset, offset);
        return;
    }

    const auto o = std::string(offset);
    ctx.AddU32("{} = bitfieldExtract(int(smem[{} >> 2]), int(({} & 3u) * 8u), 8);", inst, o, o);
}

void EmitLoadSharedU16(EmitContext& ctx, IR::Inst& inst, std::string_view offset) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = bitfieldExtract(uint(nx_shared_load32({})), int(({} & 2u) * 8u), 16);",
                   inst, offset, offset);
        return;
    }

    const auto o = std::string(offset);
    ctx.AddU32("{} = bitfieldExtract(uint(smem[{} >> 2]), int(({} & 2u) * 8u), 16);", inst, o, o);
}

void EmitLoadSharedS16(EmitContext& ctx, IR::Inst& inst, std::string_view offset) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = bitfieldExtract(int(nx_shared_load32({})), int(({} & 2u) * 8u), 16);",
                   inst, offset, offset);
        return;
    }

    const auto o = std::string(offset);
    ctx.AddU32("{} = bitfieldExtract(int(smem[{} >> 2]), int(({} & 2u) * 8u), 16);", inst, o, o);
}

void EmitLoadSharedU32(EmitContext& ctx, IR::Inst& inst, std::string_view offset) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = uint(nx_shared_load32({}));", inst, offset);
        return;
    }

    const auto o = std::string(offset);
    ctx.AddU32("{} = smem[{} >> 2];", inst, o);
}

void EmitLoadSharedU64(EmitContext& ctx, IR::Inst& inst, std::string_view offset) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32x2("{} = uint2(nx_shared_load32({} + 0u), nx_shared_load32({} + 4u));", inst,
                     offset, offset);
        return;
    }

    const auto o = std::string(offset);
    ctx.AddU32x2("{} = uint2(smem[({} + 0u) >> 2], smem[({} + 4u) >> 2]);", inst, o, o);
}

void EmitLoadSharedU128(EmitContext& ctx, IR::Inst& inst, std::string_view offset) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32x4("{} = uint4(nx_shared_load32({} + 0u), nx_shared_load32({} + 4u), "
                     "nx_shared_load32({} + 8u), nx_shared_load32({} + 12u));",
                     inst, offset, offset, offset, offset);
        return;
    }

    const auto o = std::string(offset);
    ctx.AddU32x4("{} = uint4(smem[({} + 0u) >> 2], smem[({} + 4u) >> 2], smem[({} + 8u) >> 2], "
                 "smem[({} + 12u) >> 2]);",
                 inst, o, o, o, o);
}

void EmitWriteSharedU8(EmitContext& ctx, std::string_view offset, std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.Add("nx_shared_write_subword({} + 0u, {}, 8u);", offset, value);
        return;
    }

    const auto o = std::string(offset);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = "
            "bitfieldInsert(old_value, {}, int(({} & 3u) * 8u), 8); "
            "InterlockedCompareExchange(smem[{} >> 2], old_value, new_value, previous); if "
            "(previous == old_value) break; old_value = previous; }} while (true); }}",
            o, value, o, o);
}

void EmitWriteSharedU16(EmitContext& ctx, std::string_view offset, std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.Add("nx_shared_write_subword({} + 0u, {}, 16u);", offset, value);
        return;
    }

    const auto o = std::string(offset);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = "
            "bitfieldInsert(old_value, {}, int(({} & 2u) * 8u), 16); "
            "InterlockedCompareExchange(smem[{} >> 2], old_value, new_value, previous); if "
            "(previous == old_value) break; old_value = previous; }} while (true); }}",
            o, value, o, o);
}

void EmitWriteSharedU32(EmitContext& ctx, std::string_view offset, std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.Add("nx_shared_write_subword({} + 0u, {}, 32u);", offset, value);
        return;
    }

    const auto o = std::string(offset);
    ctx.Add("smem[({} + 0u) >> 2] = {};", o, value);
}

void EmitWriteSharedU64(EmitContext& ctx, std::string_view offset, std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.Add("nx_shared_write_subword({} + 0u, {}, 32u);", offset,
                fmt::format("{}.{}", value, "xyzw"[0]));
        ctx.Add("nx_shared_write_subword({} + 4u, {}, 32u);", offset,
                fmt::format("{}.{}", value, "xyzw"[1]));
        return;
    }

    const auto o = std::string(offset);
    ctx.Add("smem[({} + 0u) >> 2] = {}.x;", o, value);
    ctx.Add("smem[({} + 4u) >> 2] = {}.y;", o, value);
}

void EmitWriteSharedU128(EmitContext& ctx, std::string_view offset, std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.Add("nx_shared_write_subword({} + 0u, {}, 32u);", offset,
                fmt::format("{}.{}", value, "xyzw"[0]));
        ctx.Add("nx_shared_write_subword({} + 4u, {}, 32u);", offset,
                fmt::format("{}.{}", value, "xyzw"[1]));
        ctx.Add("nx_shared_write_subword({} + 8u, {}, 32u);", offset,
                fmt::format("{}.{}", value, "xyzw"[2]));
        ctx.Add("nx_shared_write_subword({} + 12u, {}, 32u);", offset,
                fmt::format("{}.{}", value, "xyzw"[3]));
        return;
    }

    const auto o = std::string(offset);
    ctx.Add("smem[({} + 0u) >> 2] = {}.x;", o, value);
    ctx.Add("smem[({} + 4u) >> 2] = {}.y;", o, value);
    ctx.Add("smem[({} + 8u) >> 2] = {}.z;", o, value);
    ctx.Add("smem[({} + 12u) >> 2] = {}.w;", o, value);
}

void EmitLoadStorageU8(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                       const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.AddU32("{} = bitfieldExtract(uint({}.Load({} & ~3u)), int(({} & 3u) * 8u), 8);", inst,
               resource, o, o);
}

void EmitLoadStorageS8(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                       const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.AddU32("{} = bitfieldExtract(int({}.Load({} & ~3u)), int(({} & 3u) * 8u), 8);", inst,
               resource, o, o);
}

void EmitLoadStorageU16(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                        const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.AddU32("{} = bitfieldExtract(uint({}.Load({} & ~3u)), int(({} & 2u) * 8u), 16);", inst,
               resource, o, o);
}

void EmitLoadStorageS16(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                        const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.AddU32("{} = bitfieldExtract(int({}.Load({} & ~3u)), int(({} & 2u) * 8u), 16);", inst,
               resource, o, o);
}

void EmitLoadStorage32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                       const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.AddU32("{} = {}.Load({} & ~3u);", inst, resource, o);
}

void EmitLoadStorage64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                       const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.AddU32x2("{} = {}.Load2({});", inst, resource, o);
}

void EmitLoadStorage128(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                        const IR::Value& offset) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.AddU32x4("{} = {}.Load4({});", inst, resource, o);
}

void EmitWriteStorageU8(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset,
                        std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.Add("{{ uint old_value = {}.Load({} & ~3u); uint previous; do {{ uint new_value = "
            "bitfieldInsert(old_value, {}, int(({} & 3u) * 8u), 8); "
            "{}.InterlockedCompareExchange({} & ~3u, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); }}",
            resource, o, value, o, resource, o);
}

void EmitWriteStorageS8(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset,
                        std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.Add("{{ uint old_value = {}.Load({} & ~3u); uint previous; do {{ uint new_value = "
            "bitfieldInsert(old_value, {}, int(({} & 3u) * 8u), 8); "
            "{}.InterlockedCompareExchange({} & ~3u, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); }}",
            resource, o, value, o, resource, o);
}

void EmitWriteStorageU16(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset,
                         std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.Add("{{ uint old_value = {}.Load({} & ~3u); uint previous; do {{ uint new_value = "
            "bitfieldInsert(old_value, {}, int(({} & 2u) * 8u), 16); "
            "{}.InterlockedCompareExchange({} & ~3u, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); }}",
            resource, o, value, o, resource, o);
}

void EmitWriteStorageS16(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset,
                         std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.Add("{{ uint old_value = {}.Load({} & ~3u); uint previous; do {{ uint new_value = "
            "bitfieldInsert(old_value, {}, int(({} & 2u) * 8u), 16); "
            "{}.InterlockedCompareExchange({} & ~3u, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); }}",
            resource, o, value, o, resource, o);
}

void EmitWriteStorage32(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset,
                        std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.Add("{}.Store({}, {});", resource, o, value);
}

void EmitWriteStorage64(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset,
                        std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.Add("{}.Store2({}, {});", resource, o, value);
}

void EmitWriteStorage128(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset,
                         std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    ctx.Add("{}.Store4({}, {});", resource, o, value);
}
void EmitLoadGlobal32(EmitContext& ctx, IR::Inst& inst, std::string_view address) {
    ctx.AddU32("{} = nx_global_load32({});", inst, address);
}

void EmitWriteGlobal32(EmitContext& ctx, std::string_view address, std::string_view value) {
    ctx.Add("nx_global_write32({}, {});", address, value);
}

void EmitLoadGlobal64(EmitContext& ctx, IR::Inst& inst, std::string_view address) {
    ctx.AddU32x2("{} = nx_global_load64({});", inst, address);
}

void EmitWriteGlobal64(EmitContext& ctx, std::string_view address, std::string_view value) {
    ctx.Add("nx_global_write64({}, {});", address, value);
}

void EmitLoadGlobal128(EmitContext& ctx, IR::Inst& inst, std::string_view address) {
    ctx.AddU32x4("{} = nx_global_load128({});", inst, address);
}

void EmitWriteGlobal128(EmitContext& ctx, std::string_view address, std::string_view value) {
    ctx.Add("nx_global_write128({}, {});", address, value);
}
void EmitLoadGlobalU8(EmitContext& ctx, IR::Inst& inst, std::string_view address) {
    ctx.AddU32("{} = nx_global_load_subword({}, 8u);", inst, address);
}
void EmitWriteGlobalU8(EmitContext& ctx, std::string_view address, std::string_view value) {
    ctx.Add("nx_global_write_subword({}, {}, 8u);", address, value);
}
void EmitLoadGlobalS8(EmitContext& ctx, IR::Inst& inst, std::string_view address) {
    ctx.AddU32("{} = bitfieldExtract(int(nx_global_load_subword({}, 8u)), 0, 8);", inst, address);
}
void EmitWriteGlobalS8(EmitContext& ctx, std::string_view address, std::string_view value) {
    ctx.Add("nx_global_write_subword({}, {}, 8u);", address, value);
}
void EmitLoadGlobalU16(EmitContext& ctx, IR::Inst& inst, std::string_view address) {
    ctx.AddU32("{} = nx_global_load_subword({}, 16u);", inst, address);
}
void EmitWriteGlobalU16(EmitContext& ctx, std::string_view address, std::string_view value) {
    ctx.Add("nx_global_write_subword({}, {}, 16u);", address, value);
}
void EmitLoadGlobalS16(EmitContext& ctx, IR::Inst& inst, std::string_view address) {
    ctx.AddU32("{} = bitfieldExtract(int(nx_global_load_subword({}, 16u)), 0, 16);", inst, address);
}
void EmitWriteGlobalS16(EmitContext& ctx, std::string_view address, std::string_view value) {
    ctx.Add("nx_global_write_subword({}, {}, 16u);", address, value);
}
} // namespace Shader::Backend::HLSL
