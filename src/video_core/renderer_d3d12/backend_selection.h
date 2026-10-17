// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string_view>

namespace D3D12 {
// Shared by the frontend's window choice and renderer settings. Unknown overrides preserve
// settings.
constexpr bool UseNativeBackend(bool selected_in_settings, std::string_view override_value) {
    if (override_value.size() != 5) {
        return selected_in_settings;
    }
    constexpr std::string_view name = "d3d12";
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = override_value[i];
        const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        if (lower != name[i]) {
            return selected_in_settings;
        }
    }
    return true;
}
} // namespace D3D12
