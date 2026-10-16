// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace EdenXbox::Ui {
// Match the PC catalogue's vertical two-tone background, rounded in RGB space.
constexpr std::uint8_t AvatarBackgroundChannel(std::uint8_t start, std::uint8_t end, unsigned row) {
    row = std::min(row, 255u);
    return static_cast<std::uint8_t>((start * (255u - row) + end * row + 127u) / 255u);
}

// WIC supplies premultiplied channels. Round the background contribution once.
constexpr std::uint8_t CompositeAvatarChannel(std::uint8_t foreground, std::uint8_t alpha,
                                              std::uint8_t background) {
    return static_cast<std::uint8_t>(
        std::min(255u, foreground + (background * (255u - alpha) + 127u) / 255u));
}
} // namespace EdenXbox::Ui
