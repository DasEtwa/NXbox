// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace EdenXbox::Accounts {
constexpr bool NeedsPicker(bool /*initialized*/, bool multi, bool launching) {
    return multi && launching;
}
} // namespace EdenXbox::Accounts
