// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace EdenXbox {
// Diagnostic only: compare both clock layers with host time, independently of presents.
class TimingProbe {
public:
    struct Sample {
        double wall_ms;
        double clock_mhz;
        double guest_mhz;
        std::uint64_t polls;
        double max_poll_gap_ms;
    };

    void Reset(std::int64_t host_ns, std::uint64_t clock_ticks, std::uint64_t guest_ticks) {
        start_ns = host_ns;
        last_poll_ns = host_ns;
        start_clock = clock_ticks;
        start_guest = guest_ticks;
        poll_count = 0;
        max_gap_ns = 0;
    }

    void Poll(std::int64_t host_ns) {
        max_gap_ns = std::max(max_gap_ns, host_ns - last_poll_ns);
        last_poll_ns = host_ns;
        ++poll_count;
    }

    Sample Read(std::int64_t host_ns, std::uint64_t clock_ticks, std::uint64_t guest_ticks) const {
        const double elapsed_us = static_cast<double>(host_ns - start_ns) / 1000.0;
        return {
            elapsed_us / 1000.0,
            elapsed_us > 0.0 ? static_cast<double>(clock_ticks - start_clock) / elapsed_us : 0.0,
            elapsed_us > 0.0 ? static_cast<double>(guest_ticks - start_guest) / elapsed_us : 0.0,
            poll_count, static_cast<double>(max_gap_ns) / 1'000'000.0};
    }

private:
    std::int64_t start_ns{};
    std::int64_t last_poll_ns{};
    std::int64_t max_gap_ns{};
    std::uint64_t start_clock{};
    std::uint64_t start_guest{};
    std::uint64_t poll_count{};
};
} // namespace EdenXbox
