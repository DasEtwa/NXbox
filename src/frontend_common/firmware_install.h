// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Core {
class System;
}
namespace FirmwareManager {

enum class InstallResult { Success, NoNCAs, FailedDelete, FailedCopy, FailedCorrupted };

// Shared desktop/UWP raw-NCA installation into the system NAND registered cache.
// The caller owns folder discovery and supplies a fully initialized filesystem controller.
InstallResult InstallFirmware(Core::System& system, const std::vector<std::filesystem::path>& files,
                              const std::function<bool(size_t, size_t)>& callback);
// Verifies a complete candidate without modifying installed content; empty means invalid.
std::string VerifyFirmware(Core::System& system, const std::vector<std::filesystem::path>& files,
                           const std::function<bool(size_t, size_t)>& callback);
// Reads only the real SystemVersion archive; never reports the synthesized fallback.
std::string InstalledVersion(Core::System& system);
} // namespace FirmwareManager
