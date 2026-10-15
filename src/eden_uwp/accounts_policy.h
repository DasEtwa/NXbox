// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace EdenXbox::Accounts {
constexpr bool NeedsSetup(bool initialized) {
    return !initialized;
}
constexpr bool NeedsPicker(bool initialized, bool multi, bool launching) {
    return NeedsSetup(initialized) || (multi && launching);
}
} // namespace EdenXbox::Accounts
