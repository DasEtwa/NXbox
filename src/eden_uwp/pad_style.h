// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace EdenXbox {
enum class PadChoice { None, Pro, JoyDual, Handheld };

/// Picks the controller type the Xbox pad presents for a game's supported NpadStyleSet bits
/// (bit 0 fullkey/Pro, bit 1 handheld, bit 2 joycon dual): Pro if allowed, else JoyDual, else
/// Handheld. None when the game allows none of them (or has not set a style set yet).
inline PadChoice ChoosePadStyle(std::uint32_t supported_raw) {
    if (supported_raw & (1u << 0))
        return PadChoice::Pro;
    if (supported_raw & (1u << 2))
        return PadChoice::JoyDual;
    if (supported_raw & (1u << 1))
        return PadChoice::Handheld;
    return PadChoice::None;
}
} // namespace EdenXbox
