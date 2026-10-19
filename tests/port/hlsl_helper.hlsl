
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
struct Input {
float4 position : SV_Position;
};
struct Output {
float4 color0 : SV_Target0;
};
Output main(Input input) {
Output output = (Output)0;
float nx_alpha = 0.0f;
bool v0 = 0;
bool shfl_in_bounds = false;
{
v0 = IsHelperLane();
nx_alpha = 0.75f;
output.color0.w = float(0.75f);
}
if (!(nx_alpha > asfloat(0x3f000000u))) discard;
return output;
}
