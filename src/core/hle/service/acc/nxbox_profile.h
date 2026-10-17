// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace Service::Account::NXbox {
inline bool ShouldMigrate(std::size_t count, std::string_view name,
                          std::span<const unsigned char> avatar,
                          std::span<const unsigned char> eden_backup) {
    return count == 1 && name == "Player" && !avatar.empty() &&
           std::equal(avatar.begin(), avatar.end(), eden_backup.begin(), eden_backup.end());
}
inline std::string AvatarRelativePath(std::string_view formatted_uuid) {
    return "system/save/8000000000000010/su/avators/" + std::string(formatted_uuid) + ".jpg";
}
} // namespace Service::Account::NXbox
