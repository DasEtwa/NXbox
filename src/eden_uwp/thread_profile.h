// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace EdenXbox::ThreadProfile {
inline bool Enabled() noexcept {
    // Runtime knobs are loaded from LocalState after crash handlers are installed.
    const auto* value = std::getenv("NXBOX_THREAD_PROFILE");
    return value && std::strcmp(value, "1") == 0;
}

// 100% means one fully occupied host core; use the measured wall interval, not timer cadence.
struct CpuSample {
    std::uint64_t creation{};
    std::uint64_t cpu{};
    std::uint64_t wall_ms{};
    bool valid{};
    double Update(std::uint64_t born, std::uint64_t ticks, std::uint64_t now) noexcept {
        double percent = 0;
        if (valid && creation == born && now > wall_ms && ticks >= cpu)
            percent = static_cast<double>(ticks - cpu) / static_cast<double>(now - wall_ms) / 100.0;
        creation = born;
        cpu = ticks;
        wall_ms = now;
        valid = true;
        return percent;
    }
};

enum class Wait { Present, Fence, Count };
struct Counter {
    std::atomic<std::uint64_t> microseconds{};
    std::atomic<std::uint64_t> calls{};
};
inline std::array<Counter, static_cast<std::size_t>(Wait::Count)> waits{};
class Scope {
public:
    explicit Scope(Wait kind_) noexcept : kind{kind_}, enabled{Enabled()} {
        if (enabled)
            start = std::chrono::steady_clock::now();
    }
    ~Scope() noexcept {
        if (!enabled)
            return;
        auto& counter = waits[static_cast<std::size_t>(kind)];
        counter.microseconds.fetch_add(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count()),
            std::memory_order_relaxed);
        counter.calls.fetch_add(1, std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Wait kind;
    bool enabled;
    std::chrono::steady_clock::time_point start{};
};
} // namespace EdenXbox::ThreadProfile
