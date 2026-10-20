// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/error_overlay.h"

#include <algorithm>
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
void RunErrorOverlay(const winrt::Windows::UI::Core::CoreWindow& window, ErrorDisplay& display,
                     const std::atomic<bool>& closed, const std::atomic<bool>& return_home) {
    Renderer renderer;
    renderer.Initialize(window, true);
    Input input(window);
    InputTransition transition(input);
    float scroll = 0.0f;
    while (!closed.load() && !return_home.load()) {
        window.Dispatcher().ProcessEvents(
            winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
        const auto current = display.Current();
        if (!current) {
            break;
        }
        input.Update();
        if (input.Pressed(Button::A) || input.Pressed(Button::B)) {
            display.Dismiss(current->id);
            break;
        }
        const auto message = Widen(current->message);
        constexpr float body_height = 375.0f;
        const float max_scroll = std::max(
            0.0f, renderer.MeasureString(message, Font::Body, 1120.0f).height - body_height);
        if (input.Pressed(Button::Down)) {
            scroll += 64.0f;
        }
        if (input.Pressed(Button::Up)) {
            scroll -= 64.0f;
        }
        scroll = std::clamp(scroll, 0.0f, max_scroll);
        renderer.BeginFrame();
        DrawSheet(renderer, D2D1::RectF(320.0f, 160.0f, 1600.0f, 920.0f));
        renderer.DrawString(Tr(Text::ErrorTitle), Font::Heading,
                            D2D1::RectF(400.0f, 210.0f, 1520.0f, 280.0f), Theme::kText);
        renderer.DrawString(Widen(current->code), Font::Code,
                            D2D1::RectF(400.0f, 290.0f, 1520.0f, 430.0f), Theme::kText);
        renderer.PushClip(D2D1::RectF(400.0f, 445.0f, 1520.0f, 820.0f));
        renderer.DrawString(
            message, Font::Body,
            D2D1::RectF(400.0f, 445.0f - scroll, 1520.0f, 820.0f + max_scroll - scroll),
            Theme::kText);
        renderer.PopClip();
        std::vector<Hint> hints{{Theme::kButtonA, L"A", Tr(Text::ErrorDismiss)},
                                {Theme::kButtonB, L"B", Tr(Text::ErrorDismiss)}};
        if (max_scroll > 0.0f) {
            hints.push_back({Theme::kText, L"↕", Tr(Text::ErrorScroll)});
        }
        DrawHints(renderer, hints);
        renderer.EndFrame();
    }
}
} // namespace EdenXbox::Ui
