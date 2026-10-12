// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"
namespace EdenXbox::Ui {
// Keeps the room joined after B so the subsequently launched game's LDN services can use it.
// The application owns Network::Shutdown(), after Core has stopped using room callbacks.
bool RunMultiplayerScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                          Input& input);
} // namespace EdenXbox::Ui
