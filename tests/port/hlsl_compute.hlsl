
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
cbuffer nx_cbuf3 : register(b0, space0) { uint4 cbuf3[4096]; };
RWByteAddressBuffer ssbo[1] : register(u0, space0);
groupshared uint smem[32];
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
uint v0 = 0, v1 = 0;
bool shfl_in_bounds = false;
{
v0 = cbuf3[1][0];
ssbo[0].InterlockedAdd(0u, v0, v1);
smem[(4u + 0u) >> 2] = v1;
GroupMemoryBarrierWithGroupSync();
ssbo[0].Store(4u, v1);
}
return;
}
