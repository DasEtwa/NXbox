
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
uint64_t packUint2x32(uint2 v) { return uint64_t(v.x) | (uint64_t(v.y) << 32); }
cbuffer nx_cbuf0 : register(b0, space0) { uint4 cbuf0[4096]; };
cbuffer nx_cbuf1 : register(b1, space0) { uint4 cbuf1[4096]; };
uint nx_cbuf_load(uint binding, uint offset) { if (offset >= 65536u) return 0u; switch(binding) {
case 0u: return cbuf0[offset >> 4][(offset >> 2) & 3u];
case 1u: return cbuf1[offset >> 4][(offset >> 2) & 3u];
default: return 0u; }}
RWByteAddressBuffer ssbo[2] : register(u0, space0);
static const uint nx_global_offsets[2] = {272u, 8u};
static const uint nx_global_cbufs[2] = {0u, 1u};
bool nx_global_find(uint64_t address, uint bytes, out uint index, out uint offset) {
index = 0u; offset = 0u;
[loop] for (uint i = 0u; i < 2u; ++i) { uint o = nx_global_offsets[i];
uint64_t base = packUint2x32(uint2(nx_cbuf_load(nx_global_cbufs[i], o + 0u) & 4294967295u, nx_cbuf_load(nx_global_cbufs[i], o + 4u))); uint64_t size = packUint2x32(uint2(nx_cbuf_load(nx_global_cbufs[i], o + 8u), nx_cbuf_load(nx_global_cbufs[i], o + 12u)));
if (address >= base && address - base <= size && size - (address - base) >= bytes) { index = i; offset = uint(address - base); return true; } }
return false; }
uint nx_global_load32(uint64_t address) { uint index, offset; if (nx_global_find(address, 4u, index, offset)) return ssbo[NonUniformResourceIndex(index)].Load(offset); return (uint)0; }
void nx_global_write32(uint64_t address, uint value) { uint index, offset; if (nx_global_find(address, 4u, index, offset)) ssbo[NonUniformResourceIndex(index)].Store(offset, value); }
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
uint v0 = 0;
bool shfl_in_bounds = false;
{
v0 = nx_global_load32((uint64_t(4096u) | (uint64_t(0u) << 32)));
nx_global_write32((uint64_t(4096u) | (uint64_t(0u) << 32)), v0);
}
return;
}
