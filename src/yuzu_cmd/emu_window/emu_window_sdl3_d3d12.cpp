// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <memory>
#include <string>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "video_core/renderer_null/renderer_null.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_d3d12.h"

#ifdef YUZU_USE_EXTERNAL_SDL3
// Include this before SDL.h to prevent the external from including a dummy
#define USING_GENERATED_CONFIG_H
#include <SDL3/SDL_config.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

EmuWindow_SDL3_D3D12::EmuWindow_SDL3_D3D12(InputCommon::InputSubsystem* input_subsystem_,
                                           Core::System& system_, bool fullscreen)
    : EmuWindow_SDL3{input_subsystem_, system_} {
    const std::string window_title = fmt::format("Eden {} | {}-{} (D3D12)", Common::g_build_name,
                                                 Common::g_scm_branch, Common::g_scm_desc);
    render_window = SDL_CreateWindow(window_title.c_str(), Layout::ScreenUndocked::Width,
                                     Layout::ScreenUndocked::Height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!render_window) {
        LOG_CRITICAL(Frontend, "Failed to create D3D12 SDL window: {}", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
    const auto properties = SDL_GetWindowProperties(render_window);
    window_info.render_surface =
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!window_info.render_surface) {
        LOG_CRITICAL(Frontend, "SDL did not provide a Win32 HWND for D3D12");
        std::exit(EXIT_FAILURE);
    }
    window_info.type = Core::Frontend::WindowSystemType::Windows;
    SetWindowIcon();

    if (fullscreen) {
        Fullscreen();
        ShowCursor(false);
    }

    OnResize();
    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);
    SDL_PumpEvents();
    LOG_INFO(Frontend, "Eden Version: {} | {}-{} (D3D12)", Common::g_build_name,
             Common::g_scm_branch, Common::g_scm_desc);
}

EmuWindow_SDL3_D3D12::~EmuWindow_SDL3_D3D12() = default;

std::unique_ptr<Core::Frontend::GraphicsContext> EmuWindow_SDL3_D3D12::CreateSharedContext() const {
    return std::make_unique<DummyContext>();
}
