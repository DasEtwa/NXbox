// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Include Winsock before Windows headers to avoid winsock.h conflicts.
#include <windows.h>
#include <winsock2.h>
#ifdef GetObject
#undef GetObject
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
#include "common/settings.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/multiplayer_screen.h"
#include "eden_uwp/multiplayer_strings.h"
#include "eden_uwp/multiplayer_theme.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"
#include "network/network.h"

namespace EdenXbox::Ui {
namespace {
using MultiplayerText = Multiplayer::Text;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using namespace std::chrono_literals;

AnnounceMultiplayerRoom::RoomList FetchRooms() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment {
        ~Apartment() {
            winrt::uninit_apartment();
        }
    } apartment;
    auto base = Settings::values.web_api_url.GetValue();
    if (!base.starts_with("https://")) {
        if (base.find("://") != std::string::npos)
            throw std::runtime_error("HTTPS required");
        base = "https://" + base;
    }
    while (base.ends_with('/'))
        base.pop_back();
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().TryAppendWithoutValidation(L"api-version", L"1");
    auto reply = AwaitBounded(
        client.GetStringAsync(winrt::Windows::Foundation::Uri(winrt::to_hstring(base + "/lobby"))),
        15s);
    if (!reply || reply->size() > 2 * 1024 * 1024)
        throw std::runtime_error("Lobby unavailable");
    const auto array = JsonObject::Parse(*reply).GetNamedArray(L"rooms");
    AnnounceMultiplayerRoom::RoomList rooms;
    for (const auto& value : array) {
        if (rooms.size() >= 512)
            break;
        try {
            const auto object = value.GetObject();
            const double port = object.GetNamedNumber(L"port");
            const double slots = object.GetNamedNumber(L"maxPlayers");
            if (object.GetNamedNumber(L"netVersion") != Network::network_version || port < 1 ||
                port > 65535 || port != std::floor(port) || slots < 1 ||
                slots > Network::MaxConcurrentConnections)
                continue;
            AnnounceMultiplayerRoom::Room room;
            room.ip = winrt::to_string(object.GetNamedString(L"address"));
            room.information.name = winrt::to_string(object.GetNamedString(L"name"));
            room.information.port = static_cast<u16>(port);
            room.information.member_slots = static_cast<u32>(slots);
            room.has_password = object.GetNamedBoolean(L"hasPassword", false);
            if (room.ip.empty() || room.ip.size() > 253 || room.information.name.size() > 256)
                continue;
            rooms.push_back(std::move(room));
        } catch (const winrt::hresult_error&) {
            // One malformed listing must not hide the other rooms.
        }
    }
    return rooms;
}
} // namespace

bool RunMultiplayerScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                          Input& input) {
    input.ConsumeUntilRelease();
    if (Network::GetRoomMember().expired() && !Network::Init())
        return false;
    const auto member = Network::GetRoomMember().lock();
    const auto room = Network::GetRoom().lock();
    if (!member || !room)
        return false;
    auto error = std::make_shared<std::atomic_bool>(false);
    auto error_handle = member->BindOnError([error](const auto&) { *error = true; });
    struct Unbind {
        std::shared_ptr<Network::RoomMember> member;
        Network::RoomMember::CallbackHandle<Network::RoomMember::Error> handle;
        ~Unbind() {
            member->Unbind(handle);
        }
    } unbind{member, error_handle};
    AnnounceMultiplayerRoom::RoomList lobbies;
    std::future<AnnounceMultiplayerRoom::RoomList> listing;
    std::future<bool> operation;
    std::wstring nickname = L"XboxPlayer", address, password, room_name = L"NXbox";
    std::optional<TextEntry> entry;
    int selected = 0;
    bool hosting = false;
    bool failed = false;
    const auto refresh = [&] {
        if (!listing.valid())
            listing = std::async(std::launch::async, FetchRooms);
    };
    refresh();
    bool closed = false;
    const auto token = window.Closed([&](auto&&, auto&&) { closed = true; });
    struct ClosedToken {
        winrt::Windows::UI::Core::CoreWindow window;
        winrt::event_token token;
        ~ClosedToken() {
            window.Closed(token);
        }
    } close_token{window, token};
    while (!closed) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        input.Update();
        if (listing.valid() && listing.wait_for(0s) == std::future_status::ready) {
            try {
                lobbies = listing.get();
                failed = false;
            } catch (...) {
                failed = true;
            }
            selected = std::min(selected, 6 + static_cast<int>(lobbies.size()));
        }
        if (operation.valid() && operation.wait_for(0s) == std::future_status::ready) {
            try {
                failed = !operation.get();
            } catch (...) {
                failed = true;
            }
        }
        const bool busy = operation.valid();
        if (entry) {
            const auto result = entry->Update(input);
            if (result != TextEntry::Result::Editing) {
                if (result == TextEntry::Result::Confirmed) {
                    auto* field = selected == 0   ? &nickname
                                  : selected == 1 ? &address
                                  : selected == 2 ? &password
                                                  : &room_name;
                    *field = entry->Text().substr(0, selected == 1 ? 253 : selected == 2 ? 64 : 32);
                }
                entry.reset();
                input.ConsumeUntilRelease();
            }
        } else if (!busy) {
            if (input.Pressed(Button::B))
                break;
            if (input.Pressed(Button::Up))
                selected = std::max(0, selected - 1);
            if (input.Pressed(Button::Down))
                selected = std::min(6 + static_cast<int>(lobbies.size()), selected + 1);
            if (input.Pressed(Button::X))
                refresh();
            if (input.Pressed(Button::Y)) {
                operation = std::async(std::launch::async, [member, room] {
                    member->Leave();
                    if (room->GetState() == Network::Room::State::Open)
                        room->Destroy();
                    return true;
                });
                hosting = false;
            }
            if (input.Pressed(Button::A)) {
                if (selected < 4) {
                    const auto field = selected == 0   ? nickname
                                       : selected == 1 ? address
                                       : selected == 2 ? password
                                                       : room_name;
                    entry.emplace(Multiplayer::Tr(static_cast<MultiplayerText>(
                                      static_cast<int>(MultiplayerText::Nickname) + selected)),
                                  field);
                    input.ConsumeUntilRelease();
                } else if (selected == 6)
                    refresh();
                else {
                    const bool create = selected == 5;
                    const auto index = selected - 7;
                    const std::string host = selected == 4 ? winrt::to_string(address)
                                             : create      ? "127.0.0.1"
                                                           : lobbies.at(index).ip;
                    const u16 port = selected >= 7 ? lobbies.at(index).information.port
                                                   : Network::DefaultRoomPort;
                    const auto nick = winrt::to_string(nickname);
                    const auto pass = winrt::to_string(password);
                    const auto name = winrt::to_string(room_name);
                    *error = false;
                    operation = std::async(std::launch::async, [=] {
                        if (nick.size() < 4 || nick.size() > 20 || host.empty())
                            return false;
                        member->Leave();
                        if (room->GetState() == Network::Room::State::Open)
                            room->Destroy();
                        if (create && !room->Create(name, "", "", port, pass, 8))
                            return false;
                        member->Join(nick, host.c_str(), port, 0, Network::NoPreferredIP, pass);
                        const auto deadline = std::chrono::steady_clock::now() + 10s;
                        while (member->GetState() == Network::RoomMember::State::Joining &&
                               std::chrono::steady_clock::now() < deadline)
                            std::this_thread::sleep_for(20ms);
                        const bool joined =
                            member->GetState() == Network::RoomMember::State::Joined ||
                            member->GetState() == Network::RoomMember::State::Moderator;
                        if (!joined) {
                            member->Leave();
                            if (create)
                                room->Destroy();
                        }
                        return joined;
                    });
                    hosting = create;
                }
            }
        }
        const auto palette = Multiplayer::SystemPalette();
        renderer.BeginFrame();
        renderer.FillRounded(D2D1::RectF(0, 0, kCanvasWidth, kCanvasHeight), 0, palette.background);
        renderer.DrawString(Multiplayer::Tr(MultiplayerText::Title), Font::ModsTitle,
                            D2D1::RectF(96, 130, 1800, 220), palette.text);
        const auto status = busy               ? MultiplayerText::Working
                            : failed || *error ? MultiplayerText::Failed
                            : hosting          ? MultiplayerText::Hosting
                                               : MultiplayerText::Empty;
        renderer.DrawString(Multiplayer::Tr(status), Font::Body, D2D1::RectF(96, 225, 1800, 315),
                            palette.secondary);
        const std::wstring rows[] = {
            std::wstring(Multiplayer::Tr(MultiplayerText::Nickname)) + L": " + nickname,
            std::wstring(Multiplayer::Tr(MultiplayerText::Address)) + L": " + address,
            std::wstring(Multiplayer::Tr(MultiplayerText::Password)) +
                (password.empty() ? L"" : L": ••••"),
            std::wstring(Multiplayer::Tr(MultiplayerText::RoomName)) + L": " + room_name,
            Multiplayer::Tr(MultiplayerText::Join),
            Multiplayer::Tr(MultiplayerText::Create),
            Multiplayer::Tr(MultiplayerText::Refresh)};
        const int first = std::max(0, selected - 6);
        for (int i = first; i <= first + 6 && i <= 6 + static_cast<int>(lobbies.size()); ++i) {
            const float y = 330.0f + (i - first) * 80.0f;
            const auto box = D2D1::RectF(96, y, 1180, y + 64);
            if (i == selected)
                renderer.StrokeRounded(box, 32, 3, palette.focus);
            renderer.DrawString(i < 7 ? rows[i] : Widen(lobbies[i - 7].information.name),
                                Font::RowTitle, D2D1::RectF(120, y + 8, 1160, y + 64),
                                palette.text);
        }
        renderer.DrawString(std::wstring(Multiplayer::Tr(MultiplayerText::Players)) + L" · " +
                                std::to_wstring(member->GetPingMs()) + L" ms",
                            Font::MetaMono, D2D1::RectF(1270, 330, 1820, 380), palette.secondary);
        int row = 0;
        for (const auto& player : member->GetMemberInformationSnapshot()) {
            if (row >= 8)
                break;
            renderer.DrawString(Widen(player.nickname), Font::RowTitle,
                                D2D1::RectF(1270, 395 + row * 60.0f, 1820, 450 + row * 60.0f),
                                palette.text);
            ++row;
        }
        renderer.DrawString(Multiplayer::Tr(MultiplayerText::Hints), Font::Hint,
                            D2D1::RectF(96, 970, 1820, 1040), palette.secondary);
        if (entry)
            entry->Draw(renderer, Clock::now());
        renderer.EndFrame();
    }
    // Futures finish before local state is released; no worker outlives its captures.
    input.ConsumeUntilRelease();
    return closed;
}
} // namespace EdenXbox::Ui
