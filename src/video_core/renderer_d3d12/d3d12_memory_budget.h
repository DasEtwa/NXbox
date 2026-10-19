// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace D3D12 {
struct TextureMemoryBudget {
    std::uint64_t expected = 320ULL << 20;
    std::uint64_t critical = 448ULL << 20;
};

// Xbox CPU, guest DRAM and GPU resources share the process commit limit. Subtract
// non-cache usage before choosing GC watermarks, leaving room for transient work.
// These are soft targets: live images and dirty readback retain the existing LRU rules.
constexpr TextureMemoryBudget TextureMemoryThresholds(std::uint64_t limit, std::uint64_t usage,
                                                      std::uint64_t cache) {
    constexpr std::uint64_t reserve = 256ULL << 20;
    constexpr std::uint64_t hysteresis = 64ULL << 20;
    const auto other = usage - (std::min)(usage, cache);
    const auto available = limit - (std::min)(limit, other);
    const auto target = available - (std::min)(available, reserve);
    TextureMemoryBudget result;
    result.critical = (std::min)(result.critical, target);
    result.expected = (std::min)(result.expected, target - (std::min)(target, hysteresis));
    return result;
}
TextureMemoryBudget NativeTextureMemoryBudget(std::uint64_t cache) noexcept;
} // namespace D3D12
