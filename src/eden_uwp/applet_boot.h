// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace EdenXbox {
inline constexpr std::string_view SwitchHomeBootTarget = "nxbox-system-home";
inline constexpr std::string_view SwitchMiiEditBootTarget = "nxbox-system-mii-edit";

// View + Menu leaves the individual Switch Minus/Plus mappings available.
class HomeButtonEdge {
public:
    bool Update(bool view, bool menu) {
        const bool pressed = view && menu;
        const bool edge = pressed && !held;
        held = pressed;
        return edge;
    }
    // Require release after resume before accepting another HOME press.
    void Reset() {
        held = true;
    }

private:
    bool held{};
};
} // namespace EdenXbox
