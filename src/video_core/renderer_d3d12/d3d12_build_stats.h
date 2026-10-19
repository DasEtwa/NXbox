// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace D3D12 {
struct PsoBuildStats {
    std::uint64_t disk_hits{}, disk_misses{}, memory_hits{}, dxc_us{}, root_us{}, driver_us{},
        driver_builds{};
};
inline thread_local PsoBuildStats* active_build_stats{};
class ScopedPsoBuildStats {
public:
    explicit ScopedPsoBuildStats(PsoBuildStats& stats) : previous{active_build_stats} {
        active_build_stats = &stats;
    }
    ~ScopedPsoBuildStats() {
        active_build_stats = previous;
    }

private:
    PsoBuildStats* previous;
};
// Build-only instrumentation: no clock reads outside a measured prewarm job.
class PsoBuildTimer {
public:
    explicit PsoBuildTimer(std::uint64_t PsoBuildStats::* field)
        : total{active_build_stats ? &(active_build_stats->*field) : nullptr},
          start{total ? std::chrono::steady_clock::now()
                      : std::chrono::steady_clock::time_point{}} {}
    ~PsoBuildTimer() {
        if (total)
            *total += std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    }

private:
    std::uint64_t* total;
    std::chrono::steady_clock::time_point start;
};
template <typename Action>
auto TimePsoDriver(Action action) {
    const PsoBuildTimer timer{&PsoBuildStats::driver_us};
    if (active_build_stats)
        ++active_build_stats->driver_builds;
    return action();
}
} // namespace D3D12
