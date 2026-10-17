// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
namespace EdenXbox {
inline constexpr auto kHomeComboWindow = std::chrono::milliseconds(600);
enum class HomeComboAction { None, Prompt, GoHome };
struct HomeComboState {
    bool held = false;
    bool armed = false;
    std::chrono::steady_clock::time_point first{};
};
// Pure transition: one activation per hold, with complete release before rearming.
inline HomeComboAction UpdateHomeCombo(HomeComboState& state, bool view, bool menu,
                                       std::chrono::steady_clock::time_point now) {
    if (state.held) {
        if (!view && !menu)
            state.held = false;
        return HomeComboAction::None;
    }
    if (!view || !menu)
        return HomeComboAction::None;
    state.held = true;
    if (state.armed && now >= state.first && now - state.first <= kHomeComboWindow) {
        state.armed = false;
        return HomeComboAction::GoHome;
    }
    state.armed = true;
    state.first = now;
    return HomeComboAction::Prompt;
}
} // namespace EdenXbox
