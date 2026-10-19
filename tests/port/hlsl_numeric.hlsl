
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

float nx_half(float value) { return f16tof32(f32tof16(value)); }
uint nx_mask(uint bits) { return bits >= 32 ? 0xffffffffu : ((1u << bits) - 1u); }
uint bitfieldExtract(uint v, int offset, int bits) {
    return bits == 0 ? 0u : ((v >> offset) & nx_mask(bits));
}
int bitfieldExtract(int v, int offset, int bits) {
    if (bits == 0) return 0;
    return int(uint(v) << (32 - offset - bits)) >> (32 - bits);
}
uint64_t packUint2x32(uint2 v) { return uint64_t(v.x) | (uint64_t(v.y) << 32); }
uint2 unpackUint2x32(uint64_t v) { return uint2(uint(v), uint(v >> 32)); }
double packDouble2x32(uint2 v) { return asdouble(v.x, v.y); }
uint2 unpackDouble2x32(double v) { uint lo, hi; asuint(v, lo, hi); return uint2(lo, hi); }
uint64_t doubleBitsToUint64(double v) { return packUint2x32(unpackDouble2x32(v)); }
double uint64BitsToDouble(uint64_t v) { return packDouble2x32(unpackUint2x32(v)); }
bool nx_isnan64(double v) {
    uint2 bits = unpackDouble2x32(v);
    return (bits.y & 0x7ff00000u) == 0x7ff00000u && ((bits.y & 0xfffffu) != 0u || bits.x != 0u);
}
double nx_abs64(double v) { uint2 bits = unpackDouble2x32(v); bits.y &= 0x7fffffffu; return packDouble2x32(bits); }
double nx_min64(double a, double b) { return a < b ? a : b; }
double nx_max64(double a, double b) { return a > b ? a : b; }
double nx_trunc64(double v) {
    uint64_t bits = doubleBitsToUint64(v);
    int exponent = int((bits >> 52) & uint64_t(0x7ffu)) - 1023;
    if (exponent < 0) return uint64BitsToDouble(bits & (uint64_t(1u) << 63));
    if (exponent >= 52) return v;
    uint64_t mask = (uint64_t(1u) << (52 - exponent)) - uint64_t(1u);
    return uint64BitsToDouble(bits & ~mask);
}
double nx_rsqrt64(double v) {
    uint2 bits = unpackDouble2x32(v);
    uint exponent = (bits.y >> 20) & 0x7ffu;
    if ((bits.y & 0x80000000u) != 0u && (bits.x != 0u || (bits.y & 0x7fffffffu) != 0u))
        return asdouble(0u, 0x7ff80000u);
    if (nx_isnan64(v)) return v;
    if (v == 0.0L) return asdouble(0u, (bits.y & 0x80000000u) | 0x7ff00000u);
    if (exponent == 0x7ffu) return 0.0L;
    double normalized = v;
    if (exponent == 0u) { normalized *= 18014398509481984.0L; bits = unpackDouble2x32(normalized); }
    int e = int((bits.y >> 20) & 0x7ffu) - 1023;
    bits.y = (bits.y & 0xfffffu) | 0x3ff00000u;
    double m = packDouble2x32(bits);
    if ((e & 1) != 0) { m *= 2.0L; --e; }
    double r = double(rsqrt(float(m)));
    r *= 1.5L - 0.5L * m * r * r;
    r *= 1.5L - 0.5L * m * r * r;
    r *= 1.5L - 0.5L * m * r * r;
    double scale = asdouble(0u, uint(1023 - e / 2) << 20);
    if (exponent == 0u) scale *= 134217728.0L;
    return r * scale;
}
double nx_floor64(double v) { double t = nx_trunc64(v); return v < t ? t - 1.0L : t; }
double nx_ceil64(double v) { double t = nx_trunc64(v); return v > t ? t + 1.0L : t; }
double nx_round64(double v) {
    double t = nx_trunc64(v);
    double distance = nx_abs64(v - t);
    if (distance > 0.5L || (distance == 0.5L && (uint64_t(nx_abs64(t)) & uint64_t(1u)) != uint64_t(0u)))
        return t + (v < 0.0L ? -1.0L : 1.0L);
    return t;
}
[numthreads(8, 4, 1)]
void main(uint3 local_id : SV_GroupThreadID, uint3 group_id : SV_GroupID) {
bool v0 = 0, v79 = 0, v161 = 0, v162 = 0, v163 = 0, v166 = 0, v167 = 0, v168 = 0, v171 = 0, v172 = 0, v173 = 0, v176 = 0, v177 = 0, v178 = 0, v181 = 0, v182 = 0, v183 = 0, v186 = 0, v187 = 0, v188 = 0, v191 = 0, v192 = 0, v193 = 0, v196 = 0, v197 = 0, v198 = 0, v201 = 0, v202 = 0, v203 = 0, v206 = 0, v207 = 0, v208 = 0, v211 = 0, v212 = 0, v213 = 0, v216 = 0, v217 = 0, v218 = 0, v220 = 0, v221 = 0, v222 = 0;
uint2 v1 = 0, v11 = 0;
uint3 v2 = 0, v13 = 0;
uint4 v3 = 0, v15 = 0;
uint v5 = 0, v7 = 0, v9 = 0, v80 = 0, v81 = 0, v82 = 0, v90 = 0, v91 = 0, v224 = 0, v225 = 0, v226 = 0, v228 = 0, v229 = 0, v230 = 0, v236 = 0, v237 = 0, v238 = 0, v240 = 0, v241 = 0, v242 = 0, v248 = 0, v278 = 0, v279 = 0, v280 = 0, v281 = 0, v282 = 0, v283 = 0;
float2 v18 = 0, v40 = 0, v49 = 0, v59 = 0;
float3 v22 = 0, v44 = 0, v50 = 0, v61 = 0;
float4 v27 = 0, v48 = 0, v51 = 0, v63 = 0;
float v28 = 0, v30 = 0, v31 = 0, v33 = 0, v34 = 0, v36 = 0, v37 = 0, v41 = 0, v45 = 0, v53 = 0, v55 = 0, v57 = 0, v84 = 0, v85 = 0, v86 = 0, v87 = 0, v93 = 0, v94 = 0, v97 = 0, v98 = 0, v102 = 0, v103 = 0, v108 = 0, v109 = 0, v111 = 0, v113 = 0, v117 = 0, v118 = 0, v121 = 0, v122 = 0, v124 = 0, v126 = 0, v128 = 0, v129 = 0, v130 = 0, v131 = 0, v132 = 0, v134 = 0, v135 = 0, v140 = 0, v141 = 0, v144 = 0, v145 = 0, v148 = 0, v149 = 0, v152 = 0, v153 = 0, v156 = 0, v157 = 0, v159 = 0, v160 = 0, v164 = 0, v165 = 0, v169 = 0, v170 = 0, v174 = 0, v175 = 0, v179 = 0, v180 = 0, v184 = 0, v185 = 0, v189 = 0, v190 = 0, v194 = 0, v195 = 0, v199 = 0, v200 = 0, v204 = 0, v205 = 0, v209 = 0, v210 = 0, v214 = 0, v215 = 0, v249 = 0, v251 = 0, v252 = 0, v254 = 0, v255 = 0, v256 = 0, v257 = 0, v258 = 0, v259 = 0, v260 = 0, v261 = 0, v262 = 0, v263 = 0, v264 = 0, v265 = 0, v266 = 0, v267 = 0, v268 = 0, v269 = 0;
double2 v64 = 0, v74 = 0;
double3 v65 = 0, v76 = 0;
double4 v66 = 0, v78 = 0;
double v68 = 0, v70 = 0, v72 = 0, v88 = 0, v95 = 0, v99 = 0, v104 = 0, v110 = 0, v112 = 0, v114 = 0, v119 = 0, v123 = 0, v125 = 0, v127 = 0, v136 = 0, v142 = 0, v146 = 0, v150 = 0, v154 = 0, v158 = 0, v253 = 0, v270 = 0, v271 = 0, v272 = 0, v273 = 0, v274 = 0, v275 = 0, v276 = 0, v277 = 0;
uint64_t v83 = 0, v92 = 0, v232 = 0, v233 = 0, v234 = 0, v244 = 0, v245 = 0, v246 = 0, v247 = 0;
bool shfl_in_bounds = false;
{
v0 = false;
v1=uint2(1u,1u);
v2=uint3(1u,1u,1u);
v3=uint4(1u,1u,1u,1u);
v5=(uint2(1u,1u)).y;
v7=(uint3(1u,1u,1u)).y;
v9=(uint4(1u,1u,1u,1u)).y;
v11=(uint2(1u,1u));v11.y=1u;
v13=(uint3(1u,1u,1u));v13.y=1u;
v15=(uint4(1u,1u,1u,1u));v15.y=1u;
v18 = float2((f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu)));
v22 = float3((f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu)));
v27 = float4((f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu)));
v28 = f16tof32(15360u & 0xffffu);
v30 = (float2(v28,v28)).y;
v31 = f16tof32(15360u & 0xffffu);
v33 = (float3(v31,v31,v31)).y;
v34 = f16tof32(15360u & 0xffffu);
v36 = (float4(v34,v34,v34,v34)).y;
v37 = f16tof32(15360u & 0xffffu);
v40=(float2(v37,v37));v40.y=(f16tof32(15360u & 0xffffu));
v41 = f16tof32(15360u & 0xffffu);
v44=(float3(v41,v41,v41));v44.y=(f16tof32(15360u & 0xffffu));
v45 = f16tof32(15360u & 0xffffu);
v48=(float4(v45,v45,v45,v45));v48.y=(f16tof32(15360u & 0xffffu));
v49=float2(1.0f,1.0f);
v50=float3(1.0f,1.0f,1.0f);
v51=float4(1.0f,1.0f,1.0f,1.0f);
v53=(float2(1.0f,1.0f)).y;
v55=(float3(1.0f,1.0f,1.0f)).y;
v57=(float4(1.0f,1.0f,1.0f,1.0f)).y;
v59=(float2(1.0f,1.0f));v59.y=1.0f;
v61=(float3(1.0f,1.0f,1.0f));v61.y=1.0f;
v63=(float4(1.0f,1.0f,1.0f,1.0f));v63.y=1.0f;
v64 = double2(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u));
v65 = double3(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u));
v66 = double4(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u));
v68 = (double2(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u))).y;
v70 = (double3(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u))).y;
v72 = (double4(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u))).y;
v74=(double2(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u)));v74.y=asdouble(0x00000000u, 0x3ff00000u);
v76=(double3(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u)));v76.y=asdouble(0x00000000u, 0x3ff00000u);
v78=(double4(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u)));v78.y=asdouble(0x00000000u, 0x3ff00000u);
v79=true?true:true;
v80 = (true ? 1u : 1u) & 0xffu;
v81 = (true ? 1u : 1u) & 0xffffu;
v82=true?1u:1u;
v83=true?(uint64_t(1u) | (uint64_t(0u) << 32)):(uint64_t(1u) | (uint64_t(0u) << 32));
v84 = f16tof32(15360u & 0xffffu);
v85 = f16tof32(15360u & 0xffffu);
v86 = nx_half(true ? v84 : v85);
v87=true?1.0f:1.0f;
v88=true?asdouble(0x00000000u, 0x3ff00000u):asdouble(0x00000000u, 0x3ff00000u);
v90 = f32tof16((f16tof32(15360u & 0xffffu)));
v91=asuint(1.0f);
v92=doubleBitsToUint64(asdouble(0x00000000u, 0x3ff00000u));
v93 = f16tof32(1u & 0xffffu);
v94=asfloat(1u);
v95=uint64BitsToDouble((uint64_t(1u) | (uint64_t(0u) << 32)));
v97=nx_half(abs((f16tof32(15360u & 0xffffu))));
v98=abs(1.0f);
v99=nx_abs64(asdouble(0x00000000u, 0x3ff00000u));
v102=nx_half((f16tof32(15360u & 0xffffu))+(f16tof32(15360u & 0xffffu)));
v103=1.0f+1.0f;
v104=asdouble(0x00000000u, 0x3ff00000u)+asdouble(0x00000000u, 0x3ff00000u);
v108=nx_half(mad((f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu)),(f16tof32(15360u & 0xffffu))));
v109=mad(1.0f,1.0f,1.0f);
v110=fma(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u));
v111=max(1.0f,1.0f);
v112=nx_max64(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u));
v113=min(1.0f,1.0f);
v114=nx_min64(asdouble(0x00000000u, 0x3ff00000u),asdouble(0x00000000u, 0x3ff00000u));
v117=nx_half((f16tof32(15360u & 0xffffu))*(f16tof32(15360u & 0xffffu)));
v118=1.0f*1.0f;
v119=asdouble(0x00000000u, 0x3ff00000u)*asdouble(0x00000000u, 0x3ff00000u);
v121=nx_half(0.f-((f16tof32(15360u & 0xffffu))));
v122=0.f-(1.0f);
v123=double(0.)-(asdouble(0x00000000u, 0x3ff00000u));
v124=(1.0f)/1.0f;
v125=1.0/asdouble(0x00000000u, 0x3ff00000u);
v126=rsqrt(1.0f);
v127 = nx_rsqrt64(asdouble(0x00000000u, 0x3ff00000u));
v128=sqrt(1.0f);
v129=sin(1.0f);
v130=exp2(1.0f);
v131=cos(1.0f);
v132=log2(1.0f);
v134=nx_half(min(max((f16tof32(15360u & 0xffffu)),0.0),1.0));
v135=min(max(1.0f,0.0),1.0);
v136=nx_min64(nx_max64(asdouble(0x00000000u, 0x3ff00000u),0.0),1.0);
v140=nx_half(min(max((f16tof32(15360u & 0xffffu)),float((f16tof32(15360u & 0xffffu)))),float((f16tof32(15360u & 0xffffu)))));
v141=min(max(1.0f,float(1.0f)),float(1.0f));
v142=nx_min64(nx_max64(asdouble(0x00000000u, 0x3ff00000u),double(asdouble(0x00000000u, 0x3ff00000u))),double(asdouble(0x00000000u, 0x3ff00000u)));
v144=nx_half(round((f16tof32(15360u & 0xffffu))));
v145=round(1.0f);
v146=nx_round64(asdouble(0x00000000u, 0x3ff00000u));
v148=nx_half(floor((f16tof32(15360u & 0xffffu))));
v149=floor(1.0f);
v150=nx_floor64(asdouble(0x00000000u, 0x3ff00000u));
v152=nx_half(ceil((f16tof32(15360u & 0xffffu))));
v153=ceil(1.0f);
v154=nx_ceil64(asdouble(0x00000000u, 0x3ff00000u));
v156=nx_half(trunc((f16tof32(15360u & 0xffffu))));
v157=trunc(1.0f);
v158=nx_trunc64(asdouble(0x00000000u, 0x3ff00000u));
v159 = f16tof32(15360u & 0xffffu);
v160 = f16tof32(15360u & 0xffffu);
v161=v159==v160&&!isnan(v159)&&!isnan(v160);
v162=1.0f==1.0f&&!isnan(1.0f)&&!isnan(1.0f);
v163=asdouble(0x00000000u, 0x3ff00000u)==asdouble(0x00000000u, 0x3ff00000u)&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v164 = f16tof32(15360u & 0xffffu);
v165 = f16tof32(15360u & 0xffffu);
v166=v164==v165||isnan(v164)||isnan(v165);
v167=1.0f==1.0f||isnan(1.0f)||isnan(1.0f);
v168=asdouble(0x00000000u, 0x3ff00000u)==asdouble(0x00000000u, 0x3ff00000u)||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v169 = f16tof32(15360u & 0xffffu);
v170 = f16tof32(15360u & 0xffffu);
v171=v169!=v170&&!isnan(v169)&&!isnan(v170);
v172=1.0f!=1.0f&&!isnan(1.0f)&&!isnan(1.0f);
v173=asdouble(0x00000000u, 0x3ff00000u)!=asdouble(0x00000000u, 0x3ff00000u)&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v174 = f16tof32(15360u & 0xffffu);
v175 = f16tof32(15360u & 0xffffu);
v176=v174!=v175||isnan(v174)||isnan(v175);
v177=1.0f!=1.0f||isnan(1.0f)||isnan(1.0f);
v178=asdouble(0x00000000u, 0x3ff00000u)!=asdouble(0x00000000u, 0x3ff00000u)||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v179 = f16tof32(15360u & 0xffffu);
v180 = f16tof32(15360u & 0xffffu);
v181=v179<v180&&!isnan(v179)&&!isnan(v180);
v182=1.0f<1.0f&&!isnan(1.0f)&&!isnan(1.0f);
v183=asdouble(0x00000000u, 0x3ff00000u)<asdouble(0x00000000u, 0x3ff00000u)&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v184 = f16tof32(15360u & 0xffffu);
v185 = f16tof32(15360u & 0xffffu);
v186=v184<v185||isnan(v184)||isnan(v185);
v187=1.0f<1.0f||isnan(1.0f)||isnan(1.0f);
v188=asdouble(0x00000000u, 0x3ff00000u)<asdouble(0x00000000u, 0x3ff00000u)||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v189 = f16tof32(15360u & 0xffffu);
v190 = f16tof32(15360u & 0xffffu);
v191=v189>v190&&!isnan(v189)&&!isnan(v190);
v192=1.0f>1.0f&&!isnan(1.0f)&&!isnan(1.0f);
v193=asdouble(0x00000000u, 0x3ff00000u)>asdouble(0x00000000u, 0x3ff00000u)&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v194 = f16tof32(15360u & 0xffffu);
v195 = f16tof32(15360u & 0xffffu);
v196=v194>v195||isnan(v194)||isnan(v195);
v197=1.0f>1.0f||isnan(1.0f)||isnan(1.0f);
v198=asdouble(0x00000000u, 0x3ff00000u)>asdouble(0x00000000u, 0x3ff00000u)||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v199 = f16tof32(15360u & 0xffffu);
v200 = f16tof32(15360u & 0xffffu);
v201=v199<=v200&&!isnan(v199)&&!isnan(v200);
v202=1.0f<=1.0f&&!isnan(1.0f)&&!isnan(1.0f);
v203=asdouble(0x00000000u, 0x3ff00000u)<=asdouble(0x00000000u, 0x3ff00000u)&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v204 = f16tof32(15360u & 0xffffu);
v205 = f16tof32(15360u & 0xffffu);
v206=v204<=v205||isnan(v204)||isnan(v205);
v207=1.0f<=1.0f||isnan(1.0f)||isnan(1.0f);
v208=asdouble(0x00000000u, 0x3ff00000u)<=asdouble(0x00000000u, 0x3ff00000u)||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v209 = f16tof32(15360u & 0xffffu);
v210 = f16tof32(15360u & 0xffffu);
v211=v209>=v210&&!isnan(v209)&&!isnan(v210);
v212=1.0f>=1.0f&&!isnan(1.0f)&&!isnan(1.0f);
v213=asdouble(0x00000000u, 0x3ff00000u)>=asdouble(0x00000000u, 0x3ff00000u)&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))&&!nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v214 = f16tof32(15360u & 0xffffu);
v215 = f16tof32(15360u & 0xffffu);
v216=v214>=v215||isnan(v214)||isnan(v215);
v217=1.0f>=1.0f||isnan(1.0f)||isnan(1.0f);
v218=asdouble(0x00000000u, 0x3ff00000u)>=asdouble(0x00000000u, 0x3ff00000u)||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u))||nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v220=isnan((f16tof32(15360u & 0xffffu)));
v221=isnan(1.0f);
v222=nx_isnan64(asdouble(0x00000000u, 0x3ff00000u));
v224 = bitfieldExtract(int((f16tof32(15360u & 0xffffu))), 0, 16);
v225 = bitfieldExtract(int(1.0f), 0, 16);
v226 = bitfieldExtract(int(asdouble(0x00000000u, 0x3ff00000u)), 0, 16);
v228 = int((f16tof32(15360u & 0xffffu)));
v229 = int(1.0f);
v230 = int(asdouble(0x00000000u, 0x3ff00000u));
v232 = int64_t((f16tof32(15360u & 0xffffu)));
v233 = int64_t(1.0f);
v234 = int64_t(asdouble(0x00000000u, 0x3ff00000u));
v236 = bitfieldExtract(uint((f16tof32(15360u & 0xffffu))), 0, 16);
v237 = bitfieldExtract(uint(1.0f), 0, 16);
v238 = bitfieldExtract(uint(asdouble(0x00000000u, 0x3ff00000u)), 0, 16);
v240 = uint((f16tof32(15360u & 0xffffu)));
v241 = uint(1.0f);
v242 = uint(asdouble(0x00000000u, 0x3ff00000u));
v244 = uint64_t((f16tof32(15360u & 0xffffu)));
v245 = uint64_t(1.0f);
v246 = uint64_t(asdouble(0x00000000u, 0x3ff00000u));
v247 = uint64_t(1u);
v248 = uint((uint64_t(1u) | (uint64_t(0u) << 32)));
v249 = nx_half(float(1.0f));
v251 = float((f16tof32(15360u & 0xffffu)));
v252 = float(asdouble(0x00000000u, 0x3ff00000u));
v253 = double(1.0f);
v254 = nx_half(float(bitfieldExtract(int(1u), 0, 8)));
v255 = nx_half(float(bitfieldExtract(int(1u), 0, 16)));
v256 = nx_half(float(int(1u)));
v257 = nx_half(float(int64_t((uint64_t(1u) | (uint64_t(0u) << 32)))));
v258 = nx_half(float((1u & 0xffu)));
v259 = nx_half(float((1u & 0xffffu)));
v260 = nx_half(float(1u));
v261 = nx_half(float((uint64_t(1u) | (uint64_t(0u) << 32))));
v262 = float(bitfieldExtract(int(1u), 0, 8));
v263 = float(bitfieldExtract(int(1u), 0, 16));
v264 = float(int(1u));
v265 = float(int64_t((uint64_t(1u) | (uint64_t(0u) << 32))));
v266 = float((1u & 0xffu));
v267 = float((1u & 0xffffu));
v268 = float(1u);
v269 = float((uint64_t(1u) | (uint64_t(0u) << 32)));
v270 = double(bitfieldExtract(int(1u), 0, 8));
v271 = double(bitfieldExtract(int(1u), 0, 16));
v272 = double(int(1u));
v273 = double(int64_t((uint64_t(1u) | (uint64_t(0u) << 32))));
v274 = double((1u & 0xffu));
v275 = double((1u & 0xffffu));
v276 = double(1u);
v277 = double((uint64_t(1u) | (uint64_t(0u) << 32)));
v278 = bitfieldExtract(uint(1u), 0, 16);
v279 = uint((1u & 0xffffu));
v280 = bitfieldExtract(uint(1u), 0, 8);
v281 = uint((1u & 0xffu));
v282 = int(bitfieldExtract(int(1u), 0, 8));
v283 = int(bitfieldExtract(int(1u), 0, 16));
}
return;
}
