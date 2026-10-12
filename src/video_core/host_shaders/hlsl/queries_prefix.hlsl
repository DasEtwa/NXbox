// SPDX-FileCopyrightText: Copyright 2015 Graham Sellers, Richard Wright Jr. and Nicholas Haemel
// SPDX-License-Identifier: MIT
// Inclusive 64-bit prefix scan, following queries_prefix_scan_sum_nosubgroups.comp.
// Each dispatch handles at most 2048 queries. The caller dispatches ordered chunks.
cbuffer NxboxRootConstants : register(b0, space1) {
    uint min_accumulation_base;
    uint max_accumulation_base;
    uint accumulation_limit;
    uint buffer_offset;
};
RWByteAddressBuffer input_buffer : register(u0, space0);
RWByteAddressBuffer output_buffer : register(u1, space0);
RWByteAddressBuffer accumulated_buffer : register(u2, space0);
groupshared uint2 shared_data[2048];
uint2 Add64(uint2 a, uint2 b) {
    uint low = a.x + b.x;
    return uint2(low, a.y + b.y + uint(low < a.x));
}
[numthreads(512, 1, 1)] void main(uint lane : SV_GroupIndex) {
    uint2 accum = accumulated_buffer.Load2(0);
    for (uint i = 0; i < 4; ++i) {
        uint index = lane * 4u + i;
        shared_data[index] = index <= accumulation_limit
                                 ? input_buffer.Load2((buffer_offset + index) * 8u)
                                 : uint2(0, 0);
    }
    GroupMemoryBarrierWithGroupSync();
    // Work-efficient tree scan; all 1024 write lanes participate in every step.
    for (uint step = 0; step < 11; ++step) {
        uint mask = (1u << step) - 1u;
        for (uint half = 0; half < 2; ++half) {
            uint id = lane + half * 512u;
            uint read_id = ((id >> step) << (step + 1u)) + mask;
            uint write_id = read_id + 1u + (id & mask);
            shared_data[write_id] = Add64(shared_data[read_id], shared_data[write_id]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint i = 0; i < 4; ++i) {
        uint index = lane * 4u + i;
        if (buffer_offset + index < min_accumulation_base)
            shared_data[index] = Add64(shared_data[index], accum);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint i = 0; i < 4; ++i) {
        uint index = lane * 4u + i;
        if (index <= accumulation_limit)
            output_buffer.Store2((buffer_offset + index) * 8u, shared_data[index]);
    }
    if (lane == 0) {
        uint2 final_value = shared_data[accumulation_limit];
        if (min_accumulation_base < accumulation_limit + 1u) {
            uint2 reset_value = shared_data[max_accumulation_base - 1u];
            final_value = Add64(final_value, Add64(~reset_value, uint2(1, 0)));
        }
        accumulated_buffer.Store2(0, final_value);
    }
}
