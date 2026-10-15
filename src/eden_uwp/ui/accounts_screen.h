// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {
// Settings entry; true means the window closed.
bool RunAccountsScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                       Input& input, const std::filesystem::path& local_state);
// Owns and releases its renderer before the library/Mesa. False cancels the launch.
bool PrepareAccounts(const winrt::Windows::UI::Core::CoreWindow& window,
                     const std::filesystem::path& local_state, bool choose_for_launch);
} // namespace EdenXbox::Ui
