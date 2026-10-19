
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
uint uaddCarry(uint a, uint b, out uint carry) { uint r = a + b; carry = r < a; return r; }

uint64_t nx_abs_int64(uint64_t v) { return int64_t(v) < int64_t(0) ? uint64_t(0) - v : v; }
uint64_t packUint2x32(uint2 v) { return uint64_t(v.x) | (uint64_t(v.y) << 32); }
uint2 unpackUint2x32(uint64_t v) { return uint2(uint(v), uint(v >> 32)); }
double packDouble2x32(uint2 v) { return asdouble(v.x, v.y); }
uint2 unpackDouble2x32(double v) { uint lo, hi; asuint(v, lo, hi); return uint2(lo, hi); }
uint64_t doubleBitsToUint64(double v) { return packUint2x32(unpackDouble2x32(v)); }
double uint64BitsToDouble(uint64_t v) { return packDouble2x32(unpackUint2x32(v)); }
double nx_abs64(double v) { uint2 bits = unpackDouble2x32(v); bits.y &= 0x7fffffffu; return packDouble2x32(bits); }
double nx_trunc64(double v) {
    uint64_t bits = doubleBitsToUint64(v);
    int exponent = int((bits >> 52) & uint64_t(0x7ffu)) - 1023;
    if (exponent < 0) return uint64BitsToDouble(bits & (uint64_t(1u) << 63));
    if (exponent >= 52) return v;
    uint64_t mask = (uint64_t(1u) << (52 - exponent)) - uint64_t(1u);
    return uint64BitsToDouble(bits & ~mask);
}
double nx_round64(double v) {
    double t = nx_trunc64(v);
    double distance = nx_abs64(v - t);
    if (distance > 0.5L || (distance == 0.5L && (uint64_t(nx_abs64(t)) & uint64_t(1u)) != uint64_t(0u)))
        return t + (v < 0.0L ? -1.0L : 1.0L);
    return t;
}
RWByteAddressBuffer ssbo[1] : register(u0, space0);
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
bool v0 = 0, v2 = 0, v3 = 0, v4 = 0, v11 = 0;
uint v1 = 0, v12 = 0;
uint64_t v6 = 0, v7 = 0;
float v10 = 0;
uint carry;
bool shfl_in_bounds = false;
{
v0=int(4294967295u)>=0?int(1u)>int(2147483647u-4294967295u):int(1u)<int(2147483647u-4294967295u);
v1=uaddCarry(4294967295u,1u,carry);
v2=carry!=0;
v3=v1==0;
v4=int(v1)<0;
ssbo[0].Store(0u, (uint(v2?1u:0u)));
v6=(uint64_t(1u) | (uint64_t(1u) << 32))+(uint64_t(2u) | (uint64_t(0u) << 32));
v7=nx_abs_int64(v6);
ssbo[0].Store2(8u, (uint2(unpackUint2x32(v6))));
v10 = float((nx_round64(asdouble(0x00000000u, 0x40040000u))));
v11=v10==2.0f&&!isnan(v10)&&!isnan(2.0f);
v12=v11?3u:4u;
}
return;
}
