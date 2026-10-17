// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

namespace Service {

// Shared by all service sessions in one System lifetime. Never records guest payloads.
class ServiceTrace final {
public:
    explicit ServiceTrace(bool enabled_) : enabled{enabled_} {}

    bool RecordCall(std::string_view service, bool tipc, std::uint32_t command,
                    std::optional<std::uint32_t> result, bool deferred,
                    std::string_view interface_name = {}) {
        if (!enabled) {
            return false;
        }
        const std::scoped_lock guard{mutex};
        return calls
            .emplace(std::string{service}, std::string{interface_name}, tipc, command, result,
                     deferred)
            .second;
    }

    bool RecordMissing(std::string_view service, bool tipc, std::uint32_t command,
                       std::string_view interface_name = {}) {
        const std::scoped_lock guard{mutex};
        return missing.emplace(std::string{service}, std::string{interface_name}, tipc, command)
            .second;
    }

private:
    const bool enabled;
    std::mutex mutex;
    std::set<std::tuple<std::string, std::string, bool, std::uint32_t, std::optional<std::uint32_t>,
                        bool>>
        calls;
    std::set<std::tuple<std::string, std::string, bool, std::uint32_t>> missing;
};

} // namespace Service
