// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <winrt/Windows.UI.Core.h>
#include "eden_uwp/error_applet.h"

namespace EdenXbox::Ui {
void RunErrorOverlay(const winrt::Windows::UI::Core::CoreWindow& window, ErrorDisplay& display,
                     const std::atomic<bool>& closed, const std::atomic<bool>& return_home);
} // namespace EdenXbox::Ui
