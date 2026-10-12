// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/hle/service/nextendo/auth.h"
#include "core/hle/service/nextendo/policy.h"

namespace Service::Nextendo {

struct FriendEntry {
    std::uint64_t pid = 0;
    std::string name;
    unsigned status = 0;
    std::uint64_t program_id = 0;
    std::array<std::uint8_t, 0xc0> app_field{};
};

struct Presence {
    std::string profile_id;
    unsigned status = 0;
    std::uint64_t program_id = 0;
    std::array<std::uint8_t, 0xc0> app_field{};
    bool pending = false;
};

using TokenProvider = std::function<std::string(const Identity&, std::uint64_t, std::string_view)>;
struct RuntimeState {
    std::mutex mutex;
    Config config;
    Identity identity;
    TokenProvider token_provider;
    std::vector<FriendEntry> friends;
    Presence presence;
};
inline RuntimeState runtime;

inline Config GetConfig() {
    std::scoped_lock lock(runtime.mutex);
    return runtime.config;
}
inline bool IsEnabled() {
    return GetConfig().enabled;
}
inline void Configure(Config config) {
    std::scoped_lock lock(runtime.mutex);
    runtime.config = std::move(config);
    if (!runtime.config.enabled) {
        runtime.friends.clear();
        runtime.presence = {};
    }
}
inline void SetIdentity(Identity identity, TokenProvider provider = {}) {
    std::scoped_lock lock(runtime.mutex);
    runtime.identity = std::move(identity);
    runtime.token_provider = std::move(provider);
    runtime.friends.clear();
    runtime.presence = {};
}
inline Identity GetIdentity(std::string_view profile_id) {
    std::scoped_lock lock(runtime.mutex);
    if (!runtime.config.enabled || !runtime.config.RoutesReady() ||
        profile_id != runtime.identity.profile_id)
        return {};
    return runtime.identity;
}
inline std::string GetIdToken(std::string_view profile_id, std::uint64_t program_id,
                              std::string_view version) {
    std::scoped_lock lock(runtime.mutex);
    if (!runtime.config.enabled || !runtime.config.RoutesReady() ||
        profile_id != runtime.identity.profile_id || !runtime.token_provider ||
        runtime.identity.pid == 0 || runtime.identity.nex_token.empty())
        return {};
    return runtime.token_provider(runtime.identity, program_id, version);
}
inline Route RouteHost(std::string_view host) {
    return ResolveHost(GetConfig(), host);
}

inline void QueuePresence(Presence presence) {
    std::scoped_lock lock(runtime.mutex);
    if (!runtime.config.enabled || presence.profile_id != runtime.identity.profile_id ||
        runtime.identity.pid == 0 || presence.status > 2)
        return;
    if (runtime.presence.profile_id == presence.profile_id &&
        runtime.presence.status == presence.status &&
        runtime.presence.program_id == presence.program_id &&
        runtime.presence.app_field == presence.app_field)
        return;
    presence.pending = true;
    runtime.presence = std::move(presence);
}

inline std::vector<FriendEntry> GetFriends(std::string_view profile_id) {
    std::scoped_lock lock(runtime.mutex);
    if (!runtime.config.enabled || profile_id != runtime.identity.profile_id)
        return {};
    return runtime.friends;
}

} // namespace Service::Nextendo
