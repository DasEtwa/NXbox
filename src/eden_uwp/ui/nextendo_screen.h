// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {
bool RunNextendoScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                       Input& input);
} // namespace EdenXbox::Ui
