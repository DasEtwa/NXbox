
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
RWByteAddressBuffer ssbo[1] : register(u0, space0);
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
uint64_t v0 = 0, v1 = 0;
bool shfl_in_bounds = false;
{
{ uint64_t old_value = ssbo[0].Load<uint64_t>(0u); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = old_value + value; ssbo[0].InterlockedCompareExchange64(0u, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v0 = old_value; }
{ uint64_t old_value = ssbo[0].Load<uint64_t>(8u); uint64_t previous; uint64_t value = (uint64_t(1u) | (uint64_t(0u) << 32)); do { uint64_t new_value = uint64_t(min(int64_t(old_value), int64_t(value))); ssbo[0].InterlockedCompareExchange64(8u, old_value, new_value, previous); if (previous == old_value) break; old_value = previous; } while (true); v1 = old_value; }
}
return;
}
