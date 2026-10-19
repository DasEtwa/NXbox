// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace D3D12 {
enum class FeedbackMode { Copy, ReadOnlyDepth };

// States are tracked for the whole resource, so a writable stencil plane also
// prevents simultaneous depth sampling even if depth writes are disabled.
constexpr FeedbackMode SelectFeedbackMode(bool depth, bool depth_writes, bool stencil_writes) {
    return depth && !depth_writes && !stencil_writes ? FeedbackMode::ReadOnlyDepth
                                                     : FeedbackMode::Copy;
}

constexpr bool CompatibleDrawStates(std::uint32_t current, std::uint32_t requested,
                                    std::uint32_t write_mask) {
    return current == requested || ((current | requested) & write_mask) == 0;
}

// A buffer read through an SRV and written through a UAV in the same draw or dispatch (guest
// storage buffers often share one host buffer). It stays in UNORDERED_ACCESS for that work: a
// buffer has no layout, so the SRV still reads its memory. Textures keep the conflict.
constexpr bool MergesIntoBufferUav(std::uint32_t current, std::uint32_t requested) {
    constexpr std::uint32_t UnorderedAccess = 0x8;   // D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    constexpr std::uint32_t ShaderRead = 0x40 | 0x80; // NON_PIXEL_ and PIXEL_SHADER_RESOURCE
    const std::uint32_t all = current | requested;
    return (all & UnorderedAccess) && (all & ShaderRead) &&
           (all & ~(UnorderedAccess | ShaderRead)) == 0;
}
} // namespace D3D12
