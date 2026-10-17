// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <winrt/Windows.UI.ViewManagement.h>
#include "eden_uwp/ui/theme.h"
namespace EdenXbox::Ui::Multiplayer {
struct Palette {
    D2D1_COLOR_F background;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F focus;
};
inline Palette SystemPalette() {
    const auto color = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
        winrt::Windows::UI::ViewManagement::UIColorType::Background);
    if (static_cast<int>(color.R) + color.G + color.B > 384) {
        // Text contrasts against white: 18.9:1 primary, 10.4:1 secondary.
        return {Theme::Rgb(0xFFFFFF), Theme::Rgb(0x171717), Theme::Rgb(0x3F3F46),
                Theme::Rgb(0x171717)};
    }
    return {Theme::kBackground, Theme::kText, Theme::kTextSecondary, Theme::kPrimaryFill};
}
} // namespace EdenXbox::Ui::Multiplayer
