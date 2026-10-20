// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace D3D12 {
// uint2 arithmetic keeps the full 64-bit counters on devices without native shader int64.
constexpr char QueryResolveShader[] = R"hlsl(
cbuffer Parameters : register(b0) {
    uint operation, first_slot, slot_count, report_type, report_stream;
};
ByteAddressBuffer results : register(t0);
ByteAddressBuffer metadata : register(t1);
RWByteAddressBuffer counters : register(u0);
RWByteAddressBuffer report : register(u1);
uint2 Add(uint2 a, uint2 b) {
    uint low = a.x + b.x;
    return uint2(low, a.y + b.y + uint(low < a.x));
}
uint2 Difference(uint2 a, uint2 b) {
    if (a.y < b.y || (a.y == b.y && a.x < b.x)) return uint2(0, 0);
    return uint2(a.x - b.x, a.y - b.y - uint(a.x < b.x));
}
uint2 Sample(uint type, uint stream, uint slot, uint4 flags) {
    uint base = slot * RESULT_STRIDE;
    if (type == ZPassPixelCount || type == ZPassPixelCount64)
        return flags.x != 0 && stream == 0 ? results.Load2(base) : uint2(0, 0);
    bool captured = (flags.w & (1u << stream)) != 0;
    uint2 written = uint2(0, 0), needed = uint2(0, 0);
    if (captured) {
        written = results.Load2(base + SO_OFFSET + stream * 16);
        needed = results.Load2(base + SO_OFFSET + stream * 16 + 8);
    }
    if (type == StreamingPrimitivesSucceeded) return written;
    if (type == StreamingPrimitivesNeeded) return needed;
    if (type == StreamingPrimitivesNeededMinusSucceeded) return Difference(needed, written);
    if (type == TotalStreamingPrimitivesNeededMinusSucceeded) {
        uint2 total = uint2(0, 0);
        if (stream != 0) return total;
        for (uint s = 0; s < 4; ++s) {
            if ((flags.w & (1u << s)) == 0) continue;
            uint offset = base + SO_OFFSET + s * 16;
            total = Add(total, Difference(results.Load2(offset + 8), results.Load2(offset)));
        }
        return total;
    }
    if (type == PrimitivesGenerated || type == VtgPrimitivesOut) {
        if (type == PrimitivesGenerated && stream != 0) return uint2(0, 0);
        if (captured) return needed;
        if (stream != 0 || (flags.w & 1u) != 0) return uint2(0, 0);
        return flags.z != 0 ? results.Load2(base + 8 + (flags.y != 0 ? 4 : 1) * 8)
                            : uint2(0, 0);
    }
    if (stream != 0 || flags.z == 0) return uint2(0, 0);
    uint field;
    switch (type) {
    case VerticesGenerated: field = 0; break;
    case VertexShaderInvocations: field = 2; break;
    case GeometryShaderInvocations: field = 3; break;
    case GeometryShaderPrimitivesGenerated: field = 4; break;
    case ClipperInvocations: field = 5; break;
    case ClipperPrimitivesGenerated: field = 6; break;
    case PixelShaderInvocations: field = 7; break;
    case TessellationInitInvocations: field = 8; break;
    case TessellationShaderInvocations: field = 9; break;
    default: return uint2(0, 0);
    }
    return results.Load2(base + 8 + field * 8);
}
[numthreads(32, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (operation == 3) {
        // Maxwell Conditional tests two words independently, unlike native 64-bit predication.
        if (index == 0) {
            uint2 words = results.Load2(first_slot);
            report.Store2(0, uint2(uint(words.x != 0 && words.y != 0), 0));
        }
        return;
    }
    if (index >= TYPE_COUNT * 4) return;
    uint type = index / 4, stream = index % 4;
    uint offset = index * 8;
    uint baseline = TYPE_COUNT * 4 * 8 + offset;
    if (operation == 0) {
        uint2 total = counters.Load2(offset);
        for (uint i = 0; i < slot_count; ++i)
            total = Add(total, Sample(type, stream, first_slot + i, metadata.Load4(i * 16)));
        counters.Store2(offset, total);
    } else if (operation == 2) {
        if (type == report_type) counters.Store2(baseline, counters.Load2(offset));
    } else if (type == report_type && stream == report_stream) {
        report.Store2(0, Difference(counters.Load2(offset), counters.Load2(baseline)));
    }
}
)hlsl";
} // namespace D3D12
