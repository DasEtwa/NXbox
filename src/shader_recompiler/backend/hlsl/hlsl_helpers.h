// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
#include "shader_recompiler/backend/hlsl/hlsl_root_constants.h"

namespace Shader::Backend::HLSL {
inline constexpr std::string_view ROOT_CONSTANT_DECLARATION = R"(
cbuffer NxboxRootConstants : register(b0, space1) {
    float4 nx_render_area;
    float nx_resolution_down_factor;
    float nx_y_direction;
    uint nx_texture_scaling_mask;
    uint nx_image_scaling_mask;
    uint nx_base_vertex;
    uint nx_base_instance;
    uint nx_draw_id;
    uint nx_reserved0;
    uint4 nx_reserved1;
};
)";
inline constexpr std::string_view ARITHMETIC_HELPERS = R"(
float nx_half(float value) { return f16tof32(f32tof16(value)); }
uint nx_mask(uint bits) { return bits >= 32 ? 0xffffffffu : ((1u << bits) - 1u); }
uint bitfieldExtract(uint v, int offset, int bits) {
    return bits == 0 ? 0u : ((v >> offset) & nx_mask(bits));
}
int bitfieldExtract(int v, int offset, int bits) {
    if (bits == 0) return 0;
    return int(uint(v) << (32 - offset - bits)) >> (32 - bits);
}
uint bitfieldInsert(uint base, uint value, int offset, int bits) {
    uint mask = nx_mask(bits) << offset;
    return (base & ~mask) | ((value << offset) & mask);
}
uint uaddCarry(uint a, uint b, out uint carry) { uint r = a + b; carry = r < a; return r; }
uint packHalf2x16(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float2 unpackHalf2x16(uint v) { return float2(f16tof32(v & 0xffffu), f16tof32(v >> 16)); }
uint packFloat2x16(float2 v) { return packHalf2x16(v); }
float2 unpackFloat2x16(uint v) { return unpackHalf2x16(v); }
)";
inline constexpr std::string_view WIDE_ARITHMETIC_HELPERS = R"(
uint64_t nx_abs_int64(uint64_t v) { return int64_t(v) < int64_t(0) ? uint64_t(0) - v : v; }
uint64_t packUint2x32(uint2 v) { return uint64_t(v.x) | (uint64_t(v.y) << 32); }
uint2 unpackUint2x32(uint64_t v) { return uint2(uint(v), uint(v >> 32)); }
double packDouble2x32(uint2 v) { return asdouble(v.x, v.y); }
uint2 unpackDouble2x32(double v) { uint lo, hi; asuint(v, lo, hi); return uint2(lo, hi); }
uint64_t doubleBitsToUint64(double v) { return packUint2x32(unpackDouble2x32(v)); }
double uint64BitsToDouble(uint64_t v) { return packDouble2x32(unpackUint2x32(v)); }
bool nx_isnan64(double v) {
    uint2 bits = unpackDouble2x32(v);
    return (bits.y & 0x7ff00000u) == 0x7ff00000u && ((bits.y & 0xfffffu) != 0u || bits.x != 0u);
}
double nx_abs64(double v) { uint2 bits = unpackDouble2x32(v); bits.y &= 0x7fffffffu; return packDouble2x32(bits); }
double nx_min64(double a, double b) { return a < b ? a : b; }
double nx_max64(double a, double b) { return a > b ? a : b; }
double nx_trunc64(double v) {
    uint64_t bits = doubleBitsToUint64(v);
    int exponent = int((bits >> 52) & uint64_t(0x7ffu)) - 1023;
    if (exponent < 0) return uint64BitsToDouble(bits & (uint64_t(1u) << 63));
    if (exponent >= 52) return v;
    uint64_t mask = (uint64_t(1u) << (52 - exponent)) - uint64_t(1u);
    return uint64BitsToDouble(bits & ~mask);
}
double nx_rsqrt64(double v) {
    uint2 bits = unpackDouble2x32(v);
    uint exponent = (bits.y >> 20) & 0x7ffu;
    if ((bits.y & 0x80000000u) != 0u && (bits.x != 0u || (bits.y & 0x7fffffffu) != 0u))
        return asdouble(0u, 0x7ff80000u);
    if (nx_isnan64(v)) return v;
    if (v == 0.0L) return asdouble(0u, (bits.y & 0x80000000u) | 0x7ff00000u);
    if (exponent == 0x7ffu) return 0.0L;
    double normalized = v;
    if (exponent == 0u) { normalized *= 18014398509481984.0L; bits = unpackDouble2x32(normalized); }
    int e = int((bits.y >> 20) & 0x7ffu) - 1023;
    bits.y = (bits.y & 0xfffffu) | 0x3ff00000u;
    double m = packDouble2x32(bits);
    if ((e & 1) != 0) { m *= 2.0L; --e; }
    double r = double(rsqrt(float(m)));
    r *= 1.5L - 0.5L * m * r * r;
    r *= 1.5L - 0.5L * m * r * r;
    r *= 1.5L - 0.5L * m * r * r;
    double scale = asdouble(0u, uint(1023 - e / 2) << 20);
    if (exponent == 0u) scale *= 134217728.0L;
    return r * scale;
}
double nx_floor64(double v) { double t = nx_trunc64(v); return v < t ? t - 1.0L : t; }
double nx_ceil64(double v) { double t = nx_trunc64(v); return v > t ? t + 1.0L : t; }
double nx_round64(double v) {
    double t = nx_trunc64(v);
    double distance = nx_abs64(v - t);
    if (distance > 0.5L || (distance == 0.5L && (uint64_t(nx_abs64(t)) & uint64_t(1u)) != uint64_t(0u)))
        return t + (v < 0.0L ? -1.0L : 1.0L);
    return t;
}
)";
// Mixed-width shared accesses use a single backing array. Narrow updates preserve
// adjacent bytes through native 64-bit CAS; separate uint/uint64 arrays would alias incorrectly.
inline constexpr std::string_view SHARED_WIDE_HELPERS = R"(
uint64_t nx_shared_snapshot(uint offset) {
    uint64_t value;
    InterlockedOr(smem64[offset >> 3], uint64_t(0u), value);
    return value;
}
uint nx_shared_load32(uint offset) {
    return uint(nx_shared_snapshot(offset) >> ((offset & 4u) * 8u));
}
void nx_shared_write_subword(uint offset, uint value, uint bits) {
    uint shift = (offset & (bits == 8u ? 7u : bits == 16u ? 6u : 4u)) * 8u;
    uint64_t mask = uint64_t(nx_mask(bits)) << shift;
    uint64_t old_value = nx_shared_snapshot(offset), previous;
    do {
        uint64_t new_value = (old_value & ~mask) | ((uint64_t(value) << shift) & mask);
        InterlockedCompareExchange(smem64[offset >> 3], old_value, new_value, previous);
        if (previous == old_value) break;
        old_value = previous;
    } while (true);
}
uint nx_shared_atomic32(uint offset, uint value, uint op) {
    uint shift = (offset & 4u) * 8u;
    uint64_t mask = uint64_t(0xffffffffu) << shift;
    uint64_t old_word = nx_shared_snapshot(offset), previous;
    uint old_value;
    do {
        old_value = uint(old_word >> shift);
        uint next_value = op == 0u ? old_value + value :
                         op == 1u ? uint(min(int(old_value), int(value))) :
                         op == 2u ? min(old_value, value) :
                         op == 3u ? uint(max(int(old_value), int(value))) :
                         op == 4u ? max(old_value, value) :
                         op == 5u ? (old_value >= value ? 0u : old_value + 1u) :
                         op == 6u ? (old_value == 0u || old_value > value ? value : old_value - 1u) :
                         op == 7u ? old_value & value :
                         op == 8u ? old_value | value :
                         op == 9u ? old_value ^ value : value;
        uint64_t new_word = (old_word & ~mask) | (uint64_t(next_value) << shift);
        InterlockedCompareExchange(smem64[offset >> 3], old_word, new_word, previous);
        if (previous == old_word) break;
        old_word = previous;
    } while (true);
    return old_value;
}
)";
// SM 6.0 cannot request a fixed wave width. Ballots are partitioned into
// Maxwell's 32-lane warps even when the host wave has 64 or 128 lanes.
inline constexpr std::string_view WAVE_HELPERS = R"(
uint nx_ballot(bool predicate) { return WaveActiveBallot(predicate)[WaveGetLaneIndex() >> 5]; }
uint nx_lane() { return WaveGetLaneIndex() & 31u; }
uint nx_shuffle(uint value, uint index, uint clamp, uint segment, uint mode, out bool valid) {
    uint lane = nx_lane();
    uint min_lane = lane & segment;
    uint max_lane = min_lane | (clamp & ~segment);
    uint source = mode == 0 ? (min_lane | (index & ~segment)) :
                  mode == 1 ? lane - index : mode == 2 ? lane + index : lane ^ index;
    valid = mode == 1 ? (int(source) >= int(max_lane)) : (source <= max_lane);
    valid = valid && source < 32u;
    uint host_source = (WaveGetLaneIndex() & ~31u) | (valid ? source : lane);
    uint result = WaveReadLaneAt(value, host_source);
    return valid ? result : value;
}
)";
inline std::string RegisterDeclaration(std::string_view type, std::string_view name, char kind,
                                       unsigned index, unsigned count = 1) {
    return std::string(type) + " " + std::string(name) +
           (count > 1 ? "[" + std::to_string(count) + "]" : "") + " : register(" + kind +
           std::to_string(index) + ", space0);\n";
}
} // namespace Shader::Backend::HLSL
