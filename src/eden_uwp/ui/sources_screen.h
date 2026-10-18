// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>

#include <d2d1.h>
#include <windows.h>

#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {

// Settings -> Shop sources: add, import and remove only.
// Returns true when the window closed.
bool RunSourcesScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                      Input& input, const std::filesystem::path& local_state);

// Owned by the library, so queue workers survive top-level tab changes.
class ShopScreen {
public:
    ShopScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window, Input& input,
               const std::filesystem::path& local_state);
    ~ShopScreen();
    void Tick(Clock::time_point now, bool active);
    void Draw(Clock::time_point now, bool navigation_focused);
    void ReloadSources();
    void SetTinfoilAvailable(bool available);
    bool TakeTinfoilRequest();
    bool TakeSourcesRequest();
    bool TakeNavigationRequest();
    bool HasDownloads() const; // Includes queued and still-joining transfers. UI thread only.
    bool Typing() const; // The search keyboard is open.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The number of sources saved in LocalState\sources.json, for the Settings row.
int CountSources(const std::filesystem::path& local_state);

} // namespace EdenXbox::Ui
