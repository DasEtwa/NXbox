// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/hle/service/nextendo/runtime.h"

namespace EdenXbox::NextendoOnline {

// Public configuration lives in LocalState/nextendo.json; credentials only in PasswordVault.
void Initialize(const std::filesystem::path& local_state);
bool SetEnabled(bool enabled);
bool SaveAccount(const Service::Nextendo::Identity& identity);
void SignOut();
Service::Nextendo::Identity LinkedAccount();
std::string CurrentProfileId();
Service::Nextendo::HttpResponse Exchange(const Service::Nextendo::HttpRequest& request);
bool RefreshFriends();
std::vector<Service::Nextendo::FriendEntry> Friends();

} // namespace EdenXbox::NextendoOnline
