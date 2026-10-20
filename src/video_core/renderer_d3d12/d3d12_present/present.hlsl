// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later
// HLSL ports of Eden's presentation kernels. FidelityFX functions are prepended
// from the vendored MIT headers by CMake, preserving AMD's implementation.
cbuffer Parameters : register(b0) {
    float4 crop;
    float2 source_size;
    float2 output_size;
    uint transform;
    uint opaque;
    float sharpness;
    uint unused;
};
Texture2DArray<float4> source : register(t0);
SamplerState linear_sampler : register(s0);
struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
Vertex VS(uint id : SV_VertexID) {
    Vertex result;
    float2 uv = float2((id << 1) & 2, id & 2);
    result.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    if (transform & 4)
        uv = float2(uv.y, 1 - uv.x);
    if (transform & 1)
        uv.x = 1 - uv.x;
    if (transform & 2)
        uv.y = 1 - uv.y;
    result.uv = lerp(crop.xy, crop.zw, uv);
    return result;
}
float4 Sample(float2 uv) {
    return source.SampleLevel(linear_sampler, float3(uv, 0), 0);
}
float4 Load(int2 pixel) {
    return source.Load(int4(clamp(pixel, int2(0, 0), int2(source_size) - 1), 0, 0));
}
float4 FinishColor(float4 color) {
    if (opaque)
        color.a = 1;
    return color;
}
float4 Bilinear(Vertex input) : SV_Target {
    return FinishColor(Sample(input.uv));
}
float4 Cubic(float x) {
    float x2 = x * x, x3 = x2 * x;
    return float4(-x + 2 * x2 - x3, 2 - 5 * x2 + 3 * x3, x + 4 * x2 - 3 * x3, -x2 + x3) * .5;
}
float4 Bicubic(Vertex input) : SV_Target {
    float2 p = input.uv * source_size - .5;
    float4 wx = Cubic(frac(p.x)), wy = Cubic(frac(p.y));
    float4 result = 0;
    // Explicit Catmull-Rom taps avoid division by zero in the four-sample
    // factorization at exact texel centers and retain its negative lobes.
    [unroll] for (int y = 0; y < 4; ++y)[unroll] for (int x = 0; x < 4; ++x) result +=
        Load(int2(floor(p)) + int2(x - 1, y - 1)) * wx[x] * wy[y];
    return FinishColor(result);
}
float4 Gaussian(Vertex input) : SV_Target {
    const float offsets[2] = {1.3846153846, 3.2307692308};
    const float weights[2] = {.3162162162, .0702702703};
    float4 result = Sample(input.uv) * .2270270270;
    [unroll] for (int i = 0; i < 2; ++i) {
        float2 d = offsets[i] / source_size;
        result += weights[i] * .25 *
                  (Sample(input.uv + float2(d.x, 0)) + Sample(input.uv - float2(d.x, 0)) +
                   Sample(input.uv + float2(0, d.y)) + Sample(input.uv - float2(0, d.y)) +
                   Sample(input.uv + d) + Sample(input.uv - d) +
                   Sample(input.uv + float2(d.x, -d.y)) + Sample(input.uv - float2(d.x, -d.y)));
    }
    return FinishColor(result);
}
float4 Area(Vertex input) : SV_Target {
    float2 destination = (transform & 4) ? output_size.yx : output_size;
    float2 range = (crop.zw - crop.xy) * source_size / destination;
    float2 begin = input.uv * source_size - range * .5, end = begin + range;
    float4 sum = 0;
    // Exact box overlap also handles a footprint contained in a single texel.
    [loop] for (int y = int(floor(begin.y)); y < int(ceil(end.y)); ++y) {
        float wy = max(0., min(end.y, float(y + 1)) - max(begin.y, float(y)));
        [loop] for (int x = int(floor(begin.x)); x < int(ceil(end.x)); ++x) {
            float wx = max(0., min(end.x, float(x + 1)) - max(begin.x, float(x)));
            sum += Load(int2(x, y)) * wx * wy;
        }
    }
    return FinishColor(sum / (range.x * range.y));
}
float4 Fxaa(Vertex input) : SV_Target {
    float2 uv = input.uv, corner = uv - .5 / source_size;
    float3 nw = Sample(corner).rgb, ne = Sample(corner + float2(1, 0) / source_size).rgb;
    float3 sw = Sample(corner + float2(0, 1) / source_size).rgb;
    float3 se = Sample(corner + 1 / source_size).rgb, center = Sample(uv).rgb;
    const float3 luma = float3(.299, .587, .114);
    float a = dot(nw, luma), b = dot(ne, luma), c = dot(sw, luma), d = dot(se, luma);
    float m = dot(center, luma), low = min(m, min(min(a, b), min(c, d)));
    float high = max(m, max(max(a, b), max(c, d)));
    float2 dir = float2(-((a + b) - (c + d)), (a + c) - (b + d));
    float reduce = max((a + b + c + d) * (.25 / 8), 1. / 128);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8., 8.) / source_size;
    float3 rgb_a =
        .5 * (Sample(uv + dir * (1. / 3 - .5)).rgb + Sample(uv + dir * (2. / 3 - .5)).rgb);
    float3 rgb_b = .5 * rgb_a + .25 * (Sample(uv - dir * .5).rgb + Sample(uv + dir * .5).rgb);
    float test = dot(rgb_b, luma);
    return float4((test < low || test > high) ? rgb_a : rgb_b, Sample(uv).a);
}
AF4 FsrEasuRF(AF2 uv) {
    return source.GatherRed(linear_sampler, float3(uv, 0));
}
AF4 FsrEasuGF(AF2 uv) {
    return source.GatherGreen(linear_sampler, float3(uv, 0));
}
AF4 FsrEasuBF(AF2 uv) {
    return source.GatherBlue(linear_sampler, float3(uv, 0));
}
AF4 FsrRcasLoadF(ASU2 p) {
    return Load(p);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
float4 Easu(Vertex input) : SV_Target {
    uint4 c0, c1, c2, c3;
    FsrEasuConOffset(c0, c1, c2, c3, (crop.z - crop.x) * source_size.x,
                     (crop.w - crop.y) * source_size.y, source_size.x, source_size.y, output_size.x,
                     output_size.y, crop.x * source_size.x, crop.y * source_size.y);
    float3 color;
    FsrEasuF(color, uint2(input.position.xy), c0, c1, c2, c3);
    // AMD EASU operates on RGB; keep layer alpha from the same cropped position.
    return float4(color, Sample(input.uv).a);
}
float4 Rcas(Vertex input) : SV_Target {
    uint4 constants;
    FsrRcasCon(constants, sharpness);
    float4 color;
    // Use transformed UVs so the final pass supports rotation, flips and viewport offsets.
    uint2 pixel = uint2(clamp(input.uv * source_size, 0.0, source_size - 1.0));
    FsrRcasF(color.r, color.g, color.b, color.a, pixel, constants);
    return FinishColor(color);
}

// SPDX-FileCopyrightText: 2020 BreadFish64
// SPDX-License-Identifier: MIT
// Port of Eden's vendored ScaleForce implementation.
float ColorDist1(float4 a, float4 b) {
    // https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.2020_conversion
    const float3 K = float3(0.2627, 0.6780, 0.0593);
    const float scaleB = float(0.5) / (float(1.0) - K.b);
    const float scaleR = float(0.5) / (float(1.0) - K.r);
    float4 diff = a - b;
    float Y = dot(diff.rgb, K);
    float Cb = scaleB * (diff.b - Y);
    float Cr = scaleR * (diff.r - Y);
    float3 YCbCr = float3(Y, Cb, Cr);
    float d = length(YCbCr);
    return d;
}

float4 ColorDist(float4 reference, float4 A, float4 B, float4 C, float4 D) {
    return float4(ColorDist1(reference, A), ColorDist1(reference, B), ColorDist1(reference, C),
                  ColorDist1(reference, D));
}

float4 Scaleforce(float2 tex_coord) {
    float4 bl = float4(Sample(tex_coord + float2(-1, -1) / source_size));
    float4 bc = float4(Sample(tex_coord + float2(0, -1) / source_size));
    float4 br = float4(Sample(tex_coord + float2(1, -1) / source_size));
    float4 cl = float4(Sample(tex_coord + float2(-1, 0) / source_size));
    float4 cc = float4(Sample(tex_coord));
    float4 cr = float4(Sample(tex_coord + float2(1, 0) / source_size));
    float4 tl = float4(Sample(tex_coord + float2(-1, 1) / source_size));
    float4 tc = float4(Sample(tex_coord + float2(0, 1) / source_size));
    float4 tr = float4(Sample(tex_coord + float2(1, 1) / source_size));

    float4 offset_tl = ColorDist(cc, tl, tc, tr, cr);
    float4 offset_br = ColorDist(cc, br, bc, bl, cl);

    // Calculate how different cc is from the texels around it
    const float plus_weight = float(1.5);
    const float cross_weight = float(1.5);
    float total_dist =
        dot(offset_tl + offset_br, float4(cross_weight, plus_weight, cross_weight, plus_weight));

    if (total_dist == float(0.0)) {
        return cc;
    } else {
        // Add together all the distances with direction taken into account
        float4 tmp = offset_tl - offset_br;
        float2 total_offset =
            tmp.wy * plus_weight + (tmp.zz + float2(-tmp.x, tmp.x)) * cross_weight;

        // When the image has thin points, they tend to split apart.
        // This is because the texels all around are different and total_offset reaches into clear
        // areas. This works pretty well to keep the offset in bounds for these cases.
        float clamp_val = length(total_offset) / total_dist;
        float2 final_offset = float2(clamp(total_offset, -clamp_val, clamp_val)) / source_size;

        return Sample(tex_coord - final_offset);
    }
}

float4 ScaleForce(Vertex input) : SV_Target {
    return FinishColor(Scaleforce(input.uv));
}
