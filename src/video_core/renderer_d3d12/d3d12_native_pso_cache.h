// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>
#include <utility>

namespace D3D12 {
// Build-time only: wrappers retain their binding/topology metadata while sharing
// a native PSO. Concurrent requests for the same identity build through one owner.
template <typename Pipeline>
class NativePsoCache {
public:
    using Key = std::array<std::uint64_t, 2>;
    template <typename Build>
    Pipeline GetOrBuild(Key key, Build build) {
        std::promise<Pipeline> promise;
        std::shared_future<Pipeline> result;
        bool owner{};
        {
            std::scoped_lock lock{mutex};
            const auto found = entries.find(key);
            if (found != entries.end()) {
                result = found->second;
            } else {
                result = promise.get_future().share();
                entries.emplace(key, result);
                owner = true;
            }
        }
        if (owner) {
            try {
                promise.set_value(build());
            } catch (...) {
                {
                    std::scoped_lock lock{mutex};
                    entries.erase(key);
                }
                // Existing waiters retain their future; a later request may retry the driver.
                promise.set_exception(std::current_exception());
            }
        }
        return result.get();
    }

private:
    std::mutex mutex;
    std::map<Key, std::shared_future<Pipeline>> entries;
};
} // namespace D3D12
