
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

uint nx_ballot(bool predicate) { return WaveActiveBallot(predicate)[WaveGetLaneIndex() >> 5]; }
uint nx_lane() { return WaveGetLaneIndex() & 31u; }
uint nx_shuffle(uint value, uint index, uint clamp, uint segment, uint mode, out bool valid) {
    uint lane = nx_lane();
    uint min_lane = lane & segment;
    uint max_lane = min_lane | (clamp & ~segment);
    uint source = mode == 0 ? (min_lane | (index & ~segment)) :
                  mode == 1 ? lane - index : mode == 2 ? lane + index : lane ^ index;
    valid = mode == 1 ? (int(source) >= int(max_lane)) : (source <= max_lane);
    valid = valid && source < 32u;
    uint host_source = (WaveGetLaneIndex() & ~31u) | (valid ? source : lane);
    uint result = WaveReadLaneAt(value, host_source);
    return valid ? result : value;
}
RWByteAddressBuffer ssbo[1] : register(u0, space0);
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
bool v0 = 0, v2 = 0, v4 = 0, v5 = 0;
uint v1 = 0, v3 = 0, v6 = 0, v7 = 0;
bool shfl_in_bounds = false;
int loop0 = 0x2000;
{
v0=true;
v1 = nx_ballot(v0);
v2 = nx_ballot(v0) == nx_ballot(true);
v3 = nx_shuffle(7u, 1u, 31u, 0u, 0u, shfl_in_bounds);
v4 = shfl_in_bounds;
v5=!v4;
uint incoming_v6 = 11u;
uint incoming_v7 = 22u;
v6 = incoming_v6;
v7 = incoming_v7;
}
[loop] while (true) {
{
}
if (v0) {
{
ssbo[0].Store(0u, v6);
uint incoming_v6 = v7;
uint incoming_v7 = v6;
v6 = incoming_v6;
v7 = incoming_v7;
}
}
if (--loop0 < 0 || !(false)) break; }
return;
}
