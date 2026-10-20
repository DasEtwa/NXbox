// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Service::VI {

inline bool BisectEnabled(const char* name) noexcept {
    const auto* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

inline bool IndependentVsyncEnabled() noexcept {
#ifdef NXBOX_UWP
    return BisectEnabled("NXBOX_VI_INDEPENDENT_VSYNC");
#else
    return false;
#endif
}

inline bool VblankProfileEnabled() noexcept {
#ifdef NXBOX_UWP
    static const bool enabled =
        BisectEnabled("NXBOX_THREAD_PROFILE") || BisectEnabled("NXBOX_D3D12_FRAME_PROFILE");
    return enabled;
#else
    return false;
#endif
}

// Shared by VI, BufferQueue and the five-second UWP reporter. No logging or GPU waits
// occur on the timer thread. Samples straddling a report can fall in adjacent windows.
class VblankTiming {
public:
    using Clock = std::chrono::steady_clock;
    using Count = std::uint64_t;
    struct Sample {
        Count ticks{}, signals{}, compositions{};
        Count late_ns_max{}, signal_gap_ns_max{}, compose_ns{}, compose_ns_max{};
        // Negative, zero, 1, 2, 3, 4, and >=5 (the emulator's speed extensions).
        std::array<Count, 7> intervals{};
    };

    void RecordTick(std::chrono::nanoseconds late) {
        ticks.fetch_add(1, std::memory_order_relaxed);
        if (late.count() > 0) {
            Max(late_ns_max, static_cast<Count>(late.count()));
        }
    }

    void RecordSignal(Clock::time_point now) {
        signals.fetch_add(1, std::memory_order_relaxed);
        const auto timestamp =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        const auto previous = last_signal_ns.exchange(timestamp, std::memory_order_relaxed);
        if (previous > 0 && timestamp > previous) {
            Max(signal_gap_ns_max, static_cast<Count>(timestamp - previous));
        }
    }

    void RecordComposition(std::chrono::nanoseconds elapsed) {
        compositions.fetch_add(1, std::memory_order_relaxed);
        const auto duration = elapsed.count() > 0 ? static_cast<Count>(elapsed.count()) : 0;
        compose_ns.fetch_add(duration, std::memory_order_relaxed);
        Max(compose_ns_max, duration);
    }

    void RecordSwapInterval(std::int32_t interval) {
        const auto bucket = interval < 0   ? 0u
                            : interval > 4 ? 6u
                                           : static_cast<unsigned>(interval) + 1;
        intervals[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    Sample ReadAndReset() {
        Sample sample;
        sample.ticks = ticks.exchange(0, std::memory_order_relaxed);
        sample.signals = signals.exchange(0, std::memory_order_relaxed);
        sample.compositions = compositions.exchange(0, std::memory_order_relaxed);
        sample.late_ns_max = late_ns_max.exchange(0, std::memory_order_relaxed);
        sample.signal_gap_ns_max = signal_gap_ns_max.exchange(0, std::memory_order_relaxed);
        sample.compose_ns = compose_ns.exchange(0, std::memory_order_relaxed);
        sample.compose_ns_max = compose_ns_max.exchange(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < intervals.size(); ++i) {
            sample.intervals[i] = intervals[i].exchange(0, std::memory_order_relaxed);
        }
        return sample;
    }

    // Called before the conductor schedules its first callback for a new system.
    void Reset() {
        ReadAndReset();
        last_signal_ns.store(0, std::memory_order_relaxed);
    }

private:
    static void Max(std::atomic<Count>& target, Count value) {
        auto previous = target.load(std::memory_order_relaxed);
        while (previous < value &&
               !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
        }
    }

    std::atomic<Count> ticks{}, signals{}, compositions{};
    std::atomic<Count> late_ns_max{}, signal_gap_ns_max{}, compose_ns{}, compose_ns_max{};
    std::atomic<std::int64_t> last_signal_ns{};
    std::array<std::atomic<Count>, 7> intervals{};
};

inline VblankTiming vblank_timing;

} // namespace Service::VI
