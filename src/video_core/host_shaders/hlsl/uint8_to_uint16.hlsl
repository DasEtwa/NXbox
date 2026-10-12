// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

cbuffer NxboxRootConstants : register(b0, space1) {
    uint index_count;
};
RWByteAddressBuffer input_buffer : register(u0, space0);
RWByteAddressBuffer output_buffer : register(u1, space0);
uint Index(uint id) {
    uint value = (input_buffer.Load(id & ~3u) >> ((id & 3u) * 8u)) & 255u;
    return value == 255u ? 65535u : value;
}
// One lane owns both halfwords, preventing read/modify/write races. The caller
// pads the raw UAV to four bytes, including an odd final index.
[numthreads(256, 1, 1)] void main(uint3 thread_id : SV_DispatchThreadID) {
    uint id = thread_id.x * 2u;
    if (id >= index_count)
        return;
    uint pair = Index(id);
    if (id + 1u < index_count)
        pair |= Index(id + 1u) << 16u;
    output_buffer.Store(id * 2u, pair);
}
