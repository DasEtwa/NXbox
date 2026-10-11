// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "common/logging.h"

namespace Service::HID {
/// NXBOX_HID_DIAG=1: logs what the guest asks of HID. Each key is written once per value change,
/// so per-frame calls stay quiet. Goes through LOG_WARNING so the default log filter keeps it.
inline void HidDiag(const std::string& key, const std::string& value) {
    static const bool enabled = [] {
        const char* env = std::getenv("NXBOX_HID_DIAG");
        return env != nullptr && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::map<std::string, std::string> last;
    const std::lock_guard lock{mutex};
    auto& previous = last[key];
    if (previous == value) {
        return;
    }
    previous = value;
    LOG_WARNING(Service_HID, "NXBOX_HID_DIAG {} {}", key, value);
}
} // namespace Service::HID
