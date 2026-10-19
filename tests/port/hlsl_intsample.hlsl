
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
Texture2D<uint4> tex0 : register(t0, space0);
SamplerState samp0 : register(s0, space0);
struct Input {
float4 position : SV_Position;
};
struct Output {
float4 color0 : SV_Target0;
};
Output main(Input input) {
Output output = (Output)0;
float2 v0 = 0;
float4 v1 = 0;
bool shfl_in_bounds = false;
{
v0=float2(0.5f,0.25f);
v1 = float4(0, 0, 0, 0); { uint4 nx_d = uint4(1, 1, 1, 1); uint nx_levels = 0; uint nx_mip = uint(max(0.0f, 0.0f)); tex0.GetDimensions(nx_mip, nx_d.x, nx_d.y, nx_levels); v1 = asfloat(tex0.Load(int3(clamp(int2(floor((v0).xy * float2(nx_d.xy))), 0, int2(nx_d.xy) - 1), int(nx_mip)))); }
output.color0.x = float((v1.x));
}
return output;
}
