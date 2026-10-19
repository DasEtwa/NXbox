
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
uint nx_mask(uint bits) { return bits >= 32 ? 0xffffffffu : ((1u << bits) - 1u); }
uint bitfieldExtract(uint v, int offset, int bits) {
    return bits == 0 ? 0u : ((v >> offset) & nx_mask(bits));
}
int bitfieldExtract(int v, int offset, int bits) {
    if (bits == 0) return 0;
    return int(uint(v) << (32 - offset - bits)) >> (32 - bits);
}
uint64_t packUint2x32(uint2 v) { return uint64_t(v.x) | (uint64_t(v.y) << 32); }
uint2 unpackUint2x32(uint64_t v) { return uint2(uint(v), uint(v >> 32)); }
groupshared uint64_t smem64[16];

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
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
uint2 v0 = 0, v2 = 0, v8 = 0, v10 = 0;
uint64_t v1 = 0;
uint v3 = 0, v4 = 0, v5 = 0, v6 = 0, v7 = 0, v12 = 0, v13 = 0, v14 = 0, v15 = 0, v16 = 0, v17 = 0, v18 = 0, v19 = 0, v20 = 0, v21 = 0, v22 = 0;
uint4 v9 = 0, v11 = 0;
bool shfl_in_bounds = false;
{
v0=uint2(1u,1u);
{ uint64_t old_value; InterlockedExchange(smem64[0u >> 3], (uint64_t(1u) | (uint64_t(0u) << 32)), old_value); v1 = old_value; }
{ uint64_t old_value; InterlockedExchange(smem64[8u >> 3], packUint2x32(v0), old_value); v2 = unpackUint2x32(old_value); }
v3 = bitfieldExtract(uint(nx_shared_load32(0u)), int((0u & 3u) * 8u), 8);
v4 = bitfieldExtract(int(nx_shared_load32(0u)), int((0u & 3u) * 8u), 8);
v5 = bitfieldExtract(uint(nx_shared_load32(0u)), int((0u & 2u) * 8u), 16);
v6 = bitfieldExtract(int(nx_shared_load32(0u)), int((0u & 2u) * 8u), 16);
v7 = uint(nx_shared_load32(0u));
v8 = uint2(nx_shared_load32(0u + 0u), nx_shared_load32(0u + 4u));
v9 = uint4(nx_shared_load32(0u + 0u), nx_shared_load32(0u + 4u), nx_shared_load32(0u + 8u), nx_shared_load32(0u + 12u));
nx_shared_write_subword(0u + 0u, 1u, 8u);
nx_shared_write_subword(0u + 0u, 1u, 16u);
nx_shared_write_subword(0u + 0u, 1u, 32u);
v10=uint2(1u,1u);
nx_shared_write_subword(0u + 0u, v10.x, 32u);
nx_shared_write_subword(0u + 4u, v10.y, 32u);
v11=uint4(1u,1u,1u,1u);
nx_shared_write_subword(0u + 0u, v11.x, 32u);
nx_shared_write_subword(0u + 4u, v11.y, 32u);
nx_shared_write_subword(0u + 8u, v11.z, 32u);
nx_shared_write_subword(0u + 12u, v11.w, 32u);
v12 = nx_shared_atomic32(0u, 1u, 0u);
v13 = nx_shared_atomic32(0u, 1u, 1u);
v14 = nx_shared_atomic32(0u, 1u, 2u);
v15 = nx_shared_atomic32(0u, 1u, 3u);
v16 = nx_shared_atomic32(0u, 1u, 4u);
v17 = nx_shared_atomic32(0u, 1u, 5u);
v18 = nx_shared_atomic32(0u, 1u, 6u);
v19 = nx_shared_atomic32(0u, 1u, 7u);
v20 = nx_shared_atomic32(0u, 1u, 8u);
v21 = nx_shared_atomic32(0u, 1u, 9u);
v22 = nx_shared_atomic32(0u, 1u, 10u);
}
return;
}
