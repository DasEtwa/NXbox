// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace EdenXbox::Ui {
// WIC supplies premultiplied channels. Round the background contribution once.
constexpr std::uint8_t CompositeAvatarChannel(std::uint8_t foreground, std::uint8_t alpha,
                                              std::uint8_t background) {
    return static_cast<std::uint8_t>(
        std::min(255u, foreground + (background * (255u - alpha) + 127u) / 255u));
}
} // namespace EdenXbox::Ui
