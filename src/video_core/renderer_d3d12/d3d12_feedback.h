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
} // namespace D3D12
