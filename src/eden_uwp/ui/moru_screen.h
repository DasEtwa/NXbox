// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include "core/hle/service/mii/types/store_data.h"
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {
// True means the window closed. In picker mode A chooses an existing saved Moru;
// X opens its editor. Cancel leaves selected empty. Never writes an account itself.
bool RunMoruScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                   Input& input, std::optional<Service::Mii::StoreData>* selected = nullptr);
} // namespace EdenXbox::Ui
