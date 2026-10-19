
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
Texture2DMS<float4> tex0 : register(t0, space0);
struct Input {
float4 position : SV_Position;
};
struct Output {
float4 color0 : SV_Target0;
};
Output main(Input input) {
Output output = (Output)0;
uint2 v0 = 0;
float4 v1 = 0, v3 = 0;
bool shfl_in_bounds = false;
{
v0=uint2(1u,2u);
v1 = tex0.Load(int2(v0), int(2u));
output.color0.x = float((v1.x));
v3 = tex0.Load(int2(v0), int(0));
output.color0.y = float((v3.x));
}
return output;
}
