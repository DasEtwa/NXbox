
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
bool v0 = 0;
bool shfl_in_bounds = false;
bool nx_returned = false;
{
v0=true;
}
if (v0) {
if (v0) {
{
ssbo[0].Store(0u, 1u);
}
nx_returned = true;
}
if (!nx_returned) {
{
ssbo[0].Store(4u, 2u);
}
}
}
if (!nx_returned) {
}
return;
return;
}
