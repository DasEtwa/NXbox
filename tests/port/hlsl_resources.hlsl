
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
Texture2D<float> tex4 : register(t4, space0);
SamplerComparisonState samp4 : register(s0, space0);
Buffer<float4> tex5 : register(t5, space0);
RWByteAddressBuffer ssbo[1] : register(u5, space0);
RWTexture2D<uint> img6 : register(u6, space0);
struct Input {
float4 position : SV_Position;
};
struct Output {
float4 color0 : SV_Target0;
};
Output main(Input input) {
Output output = (Output)0;
float2 v0 = 0;
float v1 = 0;
uint4 v2 = 0, v5 = 0, v6 = 0;
float4 v3 = 0;
uint2 v4 = 0;
uint v7 = 0;
bool shfl_in_bounds = false;
{
v0=float2(0.5f,0.25f);
v1 = tex4.SampleCmpLevelZero(samp4, v0, 0.5f);
output.color0.x = float(v1);
v2 = uint4(0, 0, 0, 0); { uint mip_count = 0; tex4.GetDimensions(uint(0u), v2.x, v2.y, mip_count); v2.w = mip_count; }
v3 = tex4.GatherCmp(samp4, v0, 0.5f);
v4=uint2(1u,2u);
v5=uint4(4u,0u,0u,1u);
img6[int2(v4)] = v5.x;
v6 = uint4(img6[int2(v4)], 0u, 0u, 1u);
InterlockedAdd(img6[int2(v4)], 3u, v7);
}
return output;
}
