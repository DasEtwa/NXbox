// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later
cbuffer NxboxRootConstants : register(b0, space1) {
    uint3 origin;
    uint bytes_per_block_log2;
    int3 destination;
    uint slice_size;
    uint block_size;
    uint x_shift;
    uint block_height;
    uint block_height_mask;
    uint block_depth;
    uint block_depth_mask;
    uint2 extent;
};
RWByteAddressBuffer input_buffer : register(u0, space0);
RWTexture3D<uint4> output_image : register(u1, space0);

uint SwizzleOffset(uint2 pos) {
    return ((pos.x & 32u) << 3u) | ((pos.y & 6u) << 5u) | ((pos.x & 16u) << 1u) |
           ((pos.y & 1u) << 4u) | (pos.x & 15u);
}
uint4 ReadTexel(uint offset) {
    switch (bytes_per_block_log2) {
    case 0:
        return uint4((input_buffer.Load(offset & ~3u) >> ((offset & 3u) * 8u)) & 255u, 0, 0, 0);
    case 1:
        return uint4((input_buffer.Load(offset & ~3u) >> ((offset & 2u) * 8u)) & 65535u, 0, 0, 0);
    case 2:
        return uint4(input_buffer.Load(offset), 0, 0, 0);
    case 3:
        return uint4(input_buffer.Load2(offset), 0, 0);
    case 4:
        return input_buffer.Load4(offset);
    }
    return uint4(0, 0, 0, 0);
}

[numthreads(8, 8, 1)] void main(uint3 thread_id : SV_DispatchThreadID) {
    if (any(thread_id.xy >= extent))
        return;
    uint width, height, depth;
    output_image.GetDimensions(width, height, depth);
    int3 coord = int3(thread_id) + destination;
    if (any(coord < 0) || any(coord >= int3(width, height, depth)))
        return;
    uint3 pos = thread_id + origin;
    pos.x <<= bytes_per_block_log2;
    uint block_y = pos.y >> 3u;
    uint offset = SwizzleOffset(pos.xy);
    offset += (pos.z >> block_depth) * slice_size;
    offset += (pos.z & block_depth_mask) << (9u + block_height);
    offset += (block_y >> block_height) * block_size;
    offset += (block_y & block_height_mask) << 9u;
    offset += (pos.x >> 6u) << x_shift;
    output_image[coord] = ReadTexel(offset);
}
