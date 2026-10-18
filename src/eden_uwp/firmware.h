// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace EdenXbox {
struct FirmwareStatus {
    std::string version;
    bool conversion_required = false;
    bool install_failed = false;
    bool home_available = false;
    bool mii_edit_available = false;
};
// Run on an MTA worker before the library, direct launch, or emulation starts.
void PrepareFirmware(const std::filesystem::path& local_state);
FirmwareStatus GetFirmwareStatus();
} // namespace EdenXbox
