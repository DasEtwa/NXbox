// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

cbuffer NxboxRootConstants : register(b0, space1) {
    uint primitive_count;
    uint base_vertex;
    uint index_shift;
    uint is_strip;
};
RWByteAddressBuffer input_buffer : register(u0, space0);
RWByteAddressBuffer output_buffer : register(u1, space0);
[numthreads(256, 1, 1)] void main(uint3 thread_id : SV_DispatchThreadID) {
    uint primitive = thread_id.x;
    if (primitive >= primitive_count)
        return;
    const uint quads[6] = {0, 1, 2, 0, 2, 3};
    const uint strip[6] = {0, 3, 1, 0, 2, 3};
    uint bits = 8u << index_shift;
    for (uint vertex = 0; vertex < 6; ++vertex) {
        uint index =
            is_strip == 0 ? primitive * 4u + quads[vertex] : primitive * 2u + strip[vertex];
        uint offset = index << index_shift;
        uint packed = input_buffer.Load(offset & ~3u);
        uint value = packed >> ((offset & 3u) * 8u);
        if (bits < 32u)
            value &= (1u << bits) - 1u;
        output_buffer.Store((primitive * 6u + vertex) * 4u, value + base_vertex);
    }
}
