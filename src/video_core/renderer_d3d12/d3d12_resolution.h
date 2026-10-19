// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace D3D12 {
// "Up" scaling also includes fractional settings (1/2 and 3/4), as in the shared cache.
struct ResolutionScale {
    std::uint32_t numerator{1};
    std::uint32_t shift{};

    float Factor() const {
        return static_cast<float>(numerator) / static_cast<float>(std::uint32_t{1} << shift);
    }
    std::uint32_t Size(std::uint32_t value) const {
        return value == 0 ? 0
                          : static_cast<std::uint32_t>(std::max(
                                std::uint64_t{1}, (std::uint64_t{value} * numerator) >> shift));
    }
    float Coordinate(float value) const {
        return value * Factor();
    }
    struct Rectangle {
        std::int32_t left, top, right, bottom;
    };
    Rectangle Rect(std::int32_t left, std::int32_t top, std::int32_t right,
                   std::int32_t bottom) const {
        const auto x = static_cast<std::int32_t>(std::floor(Coordinate(static_cast<float>(left))));
        const auto y = static_cast<std::int32_t>(std::floor(Coordinate(static_cast<float>(top))));
        return {x, y,
                right <= left
                    ? x
                    : static_cast<std::int32_t>(std::ceil(Coordinate(static_cast<float>(right)))),
                bottom <= top
                    ? y
                    : static_cast<std::int32_t>(std::ceil(Coordinate(static_cast<float>(bottom))))};
    }
    // Use the actual host extent: odd guest sizes lose a fractional texel during allocation.
    static float PresentCoordinate(float value, std::uint32_t guest, std::uint32_t host) {
        return value * static_cast<float>(host) / static_cast<float>(guest);
    }
};
} // namespace D3D12
