// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>

namespace D3D12 {

constexpr size_t RuntimePsoThreadCount(bool parallel_dxc) {
    return parallel_dxc ? 3 : 2;
}

// A per-request timeout multiplies by every first-use PSO in a frame. Share one deadline
// across graphics and compute instead, including time spent preparing subsequent requests.
// The GPU thread owns this budget and resets it at the renderer's frame boundary.
class PsoFrameBudget {
public:
    using Clock = std::chrono::steady_clock;

    Clock::duration Remaining(Clock::duration requested, Clock::time_point now = Clock::now()) {
        if (!deadline)
            deadline = now + std::chrono::milliseconds{16};
        return std::clamp(requested, Clock::duration::zero(),
                          std::max(Clock::duration::zero(), *deadline - now));
    }

    void Reset() {
        deadline.reset();
    }

private:
    std::optional<Clock::time_point> deadline;
};

} // namespace D3D12
