
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
uint vertex_id : SV_VertexID; uint instance_id : SV_InstanceID;
float4 attr0 : TEXCOORD0;
};
struct Output {
float4 position : SV_Position;
};
Output main(Input input) {
Output output = (Output)0;
output.position = float4(0, 0, 0, 1);
bool shfl_in_bounds = false;
{
output.position.x = ((float(input.attr0.x))+2.0f);
}
return output;
}
