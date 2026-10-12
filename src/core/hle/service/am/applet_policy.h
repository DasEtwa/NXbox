// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/hle/service/am/am_types.h"

namespace Service::AM {

// Firmware-first policy for local applets. Account authentication and the shop are excluded.
constexpr bool PreferLocalFirmwareApplet(AppletId id) {
    switch (id) {
    case AppletId::Auth: // Parental controls, not Nintendo account authentication.
    case AppletId::Cabinet:
    case AppletId::Controller:
    case AppletId::Error:
    case AppletId::NetConnect:
    case AppletId::ProfileSelect:
    case AppletId::SoftwareKeyboard:
    case AppletId::MiiEdit:
    case AppletId::Web:
    case AppletId::PhotoViewer:
    case AppletId::OfflineWeb:
    case AppletId::WebAuth: // Captive portal, not console account authentication.
        return true;
    default:
        return false;
    }
}

// Keep creation and fallback ordering identical for both library-applet IPC variants.
template <typename GuestFactory, typename FrontendFactory>
auto CreateAppletWithFallback(bool prefer_guest, GuestFactory create_guest,
                              FrontendFactory create_frontend) {
    auto applet = prefer_guest ? create_guest() : decltype(create_guest()){};
    if (!applet) {
        return create_frontend();
    }
    return applet;
}

constexpr bool ShouldRecoverGuestApplet(bool frontend, bool completed, bool returned,
                                        bool output_sent, bool exit_requested, bool has_input) {
    return !frontend && !completed && !returned && !output_sent && !exit_requested && has_input;
}

} // namespace Service::AM
