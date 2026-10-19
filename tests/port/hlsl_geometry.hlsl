
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
float4 position : SV_Position;
};
[maxvertexcount(3)]
[instance(1)]
void main(triangle Input inputs[3], uint nx_primitive_id : SV_PrimitiveID, uint nx_invocation_id : SV_GSInstanceID, inout TriangleStream<Output> nx_stream) {
Input input = inputs[0];
Output output = (Output)0; output.position = float4(0, 0, 0, 1);
uint v1 = 0;
bool shfl_in_bounds = false;
{
output.position.x = (float(inputs[2u].position.x));
v1 = nx_invocation_id;
{ Output nx_vertex = output;
nx_stream.Append(nx_vertex); }
nx_stream.RestartStrip();
}
return;
}
