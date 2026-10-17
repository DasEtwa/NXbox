// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/nextendo_screen.h"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include "common/scope_exit.h"
#include "core/hle/service/nextendo/crypto.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/nextendo_online.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
bool RunNextendoScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                       Input& input) {
    namespace nx = Service::Nextendo;
    bool closed = false;
    const auto token = window.Closed([&](const auto&, const auto&) { closed = true; });
    SCOPE_EXIT {
        window.Closed(token);
    };
    std::unique_ptr<TextEntry> keyboard;
    std::unique_ptr<nx::Authorization> authorization;
    std::future<Text> task;
    Text feedback = Text::NextendoInstructions;
    // All network/vault operations run off the UI thread, and errors are fixed localized keys.
    const auto run = [&](auto work) {
        task = std::async(std::launch::async, [work] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            SCOPE_EXIT {
                winrt::uninit_apartment();
            };
            try {
                return work();
            } catch (...) {
                return Text::NextendoFailed;
            }
        });
    };
    while (!closed) {
        window.Dispatcher().ProcessEvents(
            winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
        input.Update();
        bool busy = task.valid();
        if (busy && task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            feedback = task.get();
            busy = false;
            input.ConsumeUntilRelease();
        }
        if (keyboard) {
            const auto result = keyboard->Update(input);
            if (result == TextEntry::Result::Cancelled) {
                authorization->Cancel();
                keyboard.reset();
                feedback = Text::NextendoInstructions;
            } else if (result == TextEntry::Result::Confirmed) {
                const auto callback = winrt::to_string(keyboard->Text());
                keyboard.reset();
                const auto profile = NextendoOnline::CurrentProfileId();
                // Move the single-use transaction into the worker, never into a persistent file.
                const auto flow = std::shared_ptr<nx::Authorization>(std::move(authorization));
                run([flow, callback, profile] {
                    const auto result =
                        flow->Complete(callback, nx::Authorization::Clock::now(), profile,
                                       NextendoOnline::Exchange, NextendoOnline::SaveAccount);
                    return result == nx::AuthResult::Ready ? Text::NextendoSignedIn
                                                           : Text::NextendoFailed;
                });
            }
        } else if (!busy) {
            if (input.Pressed(Button::B))
                return false;
            if (input.Pressed(Button::A)) {
                feedback = NextendoOnline::SetEnabled(!nx::IsEnabled()) ? Text::NextendoInstructions
                                                                        : Text::NextendoFailed;
            }
            if (input.Pressed(Button::X)) {
                if (NextendoOnline::LinkedAccount().pid != 0) {
                    run([] {
                        NextendoOnline::SignOut();
                        return Text::NextendoSignedOut;
                    });
                } else {
                    const auto config = nx::GetConfig();
                    if (!config.RoutesReady() || config.client_id.empty()) {
                        feedback = Text::NextendoNotConfigured;
                    } else {
                        try {
                            const auto verifier = nx::RandomUrl(32);
                            authorization = std::make_unique<nx::Authorization>(
                                config.client_id, verifier, nx::PkceChallenge(verifier),
                                nx::RandomUrl(24), nx::Authorization::Clock::now());
                            const auto url = authorization->AuthorizeUrl();
                            run([url] {
                                const auto operation =
                                    winrt::Windows::System::Launcher::LaunchUriAsync(
                                        winrt::Windows::Foundation::Uri(winrt::to_hstring(url)));
                                const auto opened =
                                    AwaitBounded(operation, std::chrono::seconds(15));
                                return opened && *opened ? Text::NextendoReturnHint
                                                         : Text::NextendoFailed;
                            });
                        } catch (...) {
                            feedback = Text::NextendoFailed;
                        }
                    }
                }
            }
            if (input.Pressed(Button::Menu) && authorization) {
                keyboard = std::make_unique<TextEntry>(
                    Tr(Text::NextendoCallbackTitle),
                    winrt::to_hstring(std::string(nx::CallbackUri) + '?').c_str());
            }
            if (input.Pressed(Button::Y)) {
                run([] {
                    return NextendoOnline::RefreshFriends() ? Text::NextendoFriendsLoaded
                                                            : Text::NextendoFailed;
                });
            }
        }
        renderer.BeginFrame();
        renderer.FillRounded(D2D1::RectF(0, 0, kCanvasWidth, kCanvasHeight), 0, Theme::kBackground);
        DrawClock(renderer);
        renderer.DrawString(Tr(Text::NextendoTitle), Font::TitleSmall,
                            D2D1::RectF(kScreenMargin, 168, 1800, 310), Theme::kText);
        renderer.DrawString(Tr(nx::IsEnabled() ? Text::NextendoOn : Text::NextendoOff),
                            Font::Heading, D2D1::RectF(kScreenMargin, 340, 1800, 400),
                            Theme::kText);
        renderer.DrawString(Tr(busy ? Text::NextendoBusy : feedback), Font::Body,
                            D2D1::RectF(kScreenMargin, 430, 1800, 640), Theme::kTextSecondary);
        const auto identity = NextendoOnline::LinkedAccount();
        const auto friends = NextendoOnline::Friends();
        std::wstring social = Widen(identity.username);
        for (std::size_t i = 0; i < friends.size() && i < 4; ++i) {
            social += L"\n" + Widen(friends[i].name) + L" · " +
                      Tr(friends[i].status ? Text::NextendoOn : Text::NextendoOff);
        }
        renderer.DrawString(social, Font::Body, D2D1::RectF(kScreenMargin, 660, 1800, 935),
                            Theme::kTextSecondary);
        DrawHints(renderer,
                  {{Theme::kButtonA, L"A", Tr(Text::NextendoToggle)},
                   {Theme::kButtonX, L"X", Tr(identity.pid ? Text::HintSignOut : Text::HintSignIn)},
                   {Theme::kButtonY, L"Y", Tr(Text::NextendoFriends)},
                   {Theme::kButtonA, L"☰", Tr(Text::NextendoEnterReturn)},
                   {Theme::kButtonB, L"B", Tr(Text::HintBack)}});
        if (keyboard)
            keyboard->Draw(renderer, Clock::now());
        renderer.EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return true;
}
} // namespace EdenXbox::Ui
