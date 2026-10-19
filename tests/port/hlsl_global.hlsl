
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
uint bitfieldInsert(uint base, uint value, int offset, int bits) {
    uint mask = nx_mask(bits) << offset;
    return (base & ~mask) | ((value << offset) & mask);
}
uint packHalf2x16(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float2 unpackHalf2x16(uint v) { return float2(f16tof32(v & 0xffffu), f16tof32(v >> 16)); }
uint64_t packUint2x32(uint2 v) { return uint64_t(v.x) | (uint64_t(v.y) << 32); }
cbuffer nx_cbuf0 : register(b0, space0) { uint4 cbuf0[4096]; };
RWByteAddressBuffer ssbo[1] : register(u0, space0);
static const uint nx_global_offsets[1] = {0u};
bool nx_global_find(uint64_t address, uint bytes, out uint index, out uint offset) {
index = 0u; offset = 0u;
[loop] for (uint i = 0u; i < 1u; ++i) { uint o = nx_global_offsets[i];
uint64_t base = packUint2x32(uint2(cbuf0[o >> 4].x & 4294967295u, cbuf0[o >> 4].y)); uint64_t size = packUint2x32(uint2(cbuf0[o >> 4].z, cbuf0[o >> 4].w));
if (address >= base && address - base <= size && size - (address - base) >= bytes) { index = i; offset = uint(address - base); return true; } }
return false; }
uint nx_global_load_subword(uint64_t address, uint bits) {
address &= ~uint64_t(bits == 16u ? 1u : 0u); uint index, offset;
if (!nx_global_find(address, bits / 8u, index, offset)) return 0u;
return bitfieldExtract(ssbo[NonUniformResourceIndex(index)].Load(offset & ~3u), int((offset & 3u) * 8u), int(bits)); }
void nx_global_write_subword(uint64_t address, uint value, uint bits) {
address &= ~uint64_t(bits == 16u ? 1u : 0u); uint index, offset;
if (!nx_global_find(address, bits / 8u, index, offset)) return;
uint aligned = offset & ~3u; uint shift = (offset & (bits == 8u ? 3u : 2u)) * 8u; uint old_value = ssbo[NonUniformResourceIndex(index)].Load(aligned); uint previous;
do { uint new_value = bitfieldInsert(old_value, value, int(shift), int(bits)); ssbo[NonUniformResourceIndex(index)].InterlockedCompareExchange(aligned, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); }
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
uint v0 = 0, v1 = 0, v2 = 0, v4 = 0, v8 = 0, v9 = 0, v10 = 0, v11 = 0, v12 = 0, v13 = 0, v14 = 0, v15 = 0, v16 = 0, v17 = 0, v18 = 0, v49 = 0, v51 = 0, v54 = 0, v56 = 0, v59 = 0, v61 = 0;
float v3 = 0, v46 = 0, v47 = 0, v52 = 0, v57 = 0;
bool v5 = 0, v6 = 0, v7 = 0;
uint64_t v19 = 0, v20 = 0, v21 = 0, v22 = 0, v23 = 0, v24 = 0, v25 = 0, v26 = 0, v27 = 0;
uint2 v28 = 0, v29 = 0, v30 = 0, v31 = 0, v32 = 0, v33 = 0, v34 = 0, v35 = 0, v36 = 0, v37 = 0, v38 = 0, v39 = 0, v40 = 0, v41 = 0, v42 = 0, v43 = 0, v44 = 0, v45 = 0;
float2 v48 = 0, v50 = 0, v53 = 0, v55 = 0, v58 = 0, v60 = 0;
bool shfl_in_bounds = false;
uint nx_regs[255] = (uint[255])0; bool nx_preds[7] = (bool[7])0; uint nx_branch = 0u; bool nx_z = false, nx_s = false, nx_c = false, nx_o = false;
bool nx_goto_17 = false;
{
v0 = bitfieldExtract(int(nx_global_load_subword((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u)), 0, 8);
nx_global_write_subword((uint64_t(4096u) | (uint64_t(0u) << 32)), v0, 16u);
v1 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAdd(nx_global_offset, v0, v1);
} }
v2 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = uint(min(int(old_value), int(v0))); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v2 = old_value; }
} }
v3 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = asuint(asfloat(old_value) + 1.0f); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v3 = asfloat(old_value); }
} }
nx_regs[0] = v0;
v4 = nx_regs[0];
ssbo[0].Store(0u, v4);
nx_preds[0] = true;
v5 = nx_preds[0];
v6 = true;
nx_goto_17 = true;
v7 = nx_goto_17;
v8 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAdd(nx_global_offset, 1u, v8);
} }
v9 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = uint(min(int(old_value), int(1u))); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v9 = old_value; }
} }
v10 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedMin(nx_global_offset, 1u, v10);
} }
v11 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = uint(max(int(old_value), int(1u))); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v11 = old_value; }
} }
v12 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedMax(nx_global_offset, 1u, v12);
} }
v13 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = old_value >= 1u ? 0u : old_value + 1u; ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v13 = old_value; }
} }
v14 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = old_value == 0u || old_value > 1u ? 1u : old_value - 1u; ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v14 = old_value; }
} }
v15 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAnd(nx_global_offset, 1u, v15);
} }
v16 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedOr(nx_global_offset, 1u, v16);
} }
v17 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedXor(nx_global_offset, 1u, v17);
} }
v18 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedExchange(nx_global_offset, 1u, v18);
} }
v19 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = old_value + value; ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v19 = old_value; }
} }
v20 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = uint64_t(min(int64_t(old_value), int64_t(value))); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v20 = old_value; }
} }
v21 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = min(old_value, value); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v21 = old_value; }
} }
v22 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = uint64_t(max(int64_t(old_value), int64_t(value))); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v22 = old_value; }
} }
v23 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = max(old_value, value); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v23 = old_value; }
} }
v24 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = old_value & value; ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v24 = old_value; }
} }
v25 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = old_value | value; ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v25 = old_value; }
} }
v26 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = old_value ^ value; ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v26 = old_value; }
} }
v27 = uint64_t(0u);
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint64_t old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load<uint64_t>(nx_global_offset); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = value; ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange64(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v27 = old_value; }
} }
v28=uint2(1u,1u);
v29 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAdd(nx_global_offset, v28.x, v29.x); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAdd(nx_global_offset + 4u, v28.y, v29.y);
} }
v30=uint2(1u,1u);
v31 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
for (uint component = 0u; component < 2u; ++component) { uint byte_offset = nx_global_offset + component * 4u; uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(byte_offset); uint previous; do { uint new_value = uint(min(int(old_value), int(v30[component]))); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v31[component] = old_value; }
} }
v32=uint2(1u,1u);
v33 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedMin(nx_global_offset, v32.x, v33.x); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedMin(nx_global_offset + 4u, v32.y, v33.y);
} }
v34=uint2(1u,1u);
v35 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
for (uint component = 0u; component < 2u; ++component) { uint byte_offset = nx_global_offset + component * 4u; uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(byte_offset); uint previous; do { uint new_value = uint(max(int(old_value), int(v34[component]))); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v35[component] = old_value; }
} }
v36=uint2(1u,1u);
v37 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedMax(nx_global_offset, v36.x, v37.x); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedMax(nx_global_offset + 4u, v36.y, v37.y);
} }
v38=uint2(1u,1u);
v39 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAnd(nx_global_offset, v38.x, v39.x); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAnd(nx_global_offset + 4u, v38.y, v39.y);
} }
v40=uint2(1u,1u);
v41 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedOr(nx_global_offset, v40.x, v41.x); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedOr(nx_global_offset + 4u, v40.y, v41.y);
} }
v42=uint2(1u,1u);
v43 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedXor(nx_global_offset, v42.x, v43.x); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedXor(nx_global_offset + 4u, v42.y, v43.y);
} }
v44=uint2(1u,1u);
v45 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedExchange(nx_global_offset, v44.x, v45.x); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedExchange(nx_global_offset + 4u, v44.y, v45.y);
} }
v46 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 4u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = asuint(asfloat(old_value) + 1.0f); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v46 = asfloat(old_value); }
} }
v47 = f16tof32(15360u & 0xffffu);
v48 = float2(v47,v47);
v49 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = packHalf2x16(unpackHalf2x16(old_value) + v48); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v49 = old_value; }
} }
v50=float2(1.0f,1.0f);
v51 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = packHalf2x16(unpackHalf2x16(old_value) + v50); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v51 = old_value; }
} }
v52 = f16tof32(15360u & 0xffffu);
v53 = float2(v52,v52);
v54 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = packHalf2x16(min(unpackHalf2x16(old_value), v53)); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v54 = old_value; }
} }
v55=float2(1.0f,1.0f);
v56 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = packHalf2x16(min(unpackHalf2x16(old_value), v55)); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v56 = old_value; }
} }
v57 = f16tof32(15360u & 0xffffu);
v58 = float2(v57,v57);
v59 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = packHalf2x16(max(unpackHalf2x16(old_value), v58)); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v59 = old_value; }
} }
v60=float2(1.0f,1.0f);
v61 = 0;
{ uint nx_global_index, nx_global_offset; if (nx_global_find((uint64_t(4096u) | (uint64_t(0u) << 32)), 8u, nx_global_index, nx_global_offset)) {
{ uint old_value = ssbo[NonUniformResourceIndex(nx_global_index)].Load(nx_global_offset); uint previous; do { uint new_value = packHalf2x16(max(unpackHalf2x16(old_value), v60)); ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedCompareExchange(nx_global_offset, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v61 = old_value; }
} }
}
return;
}
