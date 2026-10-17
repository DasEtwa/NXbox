// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace EdenXbox::Accounts {
struct State {
    bool initialized = false;
    bool multi = false;
    std::string selected_uuid;
};
State Load(const std::filesystem::path& local_state);
bool Save(const std::filesystem::path& local_state, const State& state);
// Apply after settings files: neither global nor per-title settings may override the account.
void Apply(const std::filesystem::path& local_state);
// Only the two account-specific save layouts; device/system/shared saves are never removed.
bool DeleteSaves(const std::string& uuid);
} // namespace EdenXbox::Accounts
