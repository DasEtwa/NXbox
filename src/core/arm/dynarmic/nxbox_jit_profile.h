// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Core::NxboxJit {
// Owned by one JIT callback interface; the scheduler serializes execution of that
// interface. No atomic increments, clock reads or file writes in memory callbacks.
struct Profile {
    using Clock = std::chrono::steady_clock;
    bool enabled{[] {
        const auto* value = std::getenv("NXBOX_THREAD_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }()};
    std::uint64_t code_reads{};
    std::uint64_t code_page_fills{};
    std::uint64_t fresh_code_reads{};
    std::uint64_t memory_callbacks{};
    Clock::time_point start{enabled ? Clock::now() : Clock::time_point{}};

    void CodeRead(bool fresh) noexcept {
        if (enabled) {
            if (fresh) {
                ++fresh_code_reads;
            } else {
                ++code_reads;
            }
        }
    }
    void PageFill() noexcept {
        if (enabled) {
            ++code_page_fills;
        }
    }
    void MemoryCallback() noexcept {
        if (enabled) {
            ++memory_callbacks;
        }
    }
    // Check only after a guest run slice. If a core is idle, report its next slice
    // with the actual elapsed interval rather than claiming a fixed 5 s sample.
    template <typename Report>
    void Poll(Report&& report, Clock::time_point now) noexcept {
        if (!enabled || now - start < std::chrono::seconds(5)) {
            return;
        }
        try {
            report(code_reads, code_page_fills, fresh_code_reads, memory_callbacks,
                   std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        } catch (...) {
            // Optional telemetry must never interrupt guest execution.
        }
        code_reads = code_page_fills = fresh_code_reads = memory_callbacks = 0;
        start = now;
    }
};
} // namespace Core::NxboxJit
