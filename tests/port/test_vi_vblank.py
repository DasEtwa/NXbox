# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production VI conductor with a deliberately blocked compositor."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

STUBS = {
    "common/common_types.h": """
#pragma once
#include <cstdint>
using s32 = std::int32_t;
using s64 = std::int64_t;
using u64 = std::uint64_t;
using f32 = float;
""",
    "common/container/unordered_map.h": """
#pragma once
#include <unordered_map>
namespace Common { template<class K, class V> using unordered_map = std::unordered_map<K, V>; }
""",
    "common/adpf.h": """
#pragma once
#include <chrono>
namespace Common::ADPF { inline void SetTargetWorkDuration(std::chrono::nanoseconds) {} }
""",
    "common/thread.h": """
#pragma once
#include <condition_variable>
#include <mutex>
#include <thread>
namespace Common {
class Event {
    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
public:
    void Set() { std::lock_guard lock{mutex}; ready = true; condition.notify_one(); }
    void Wait() {
        std::unique_lock lock{mutex};
        condition.wait(lock, [&] { return ready; });
        ready = false;
    }
};
enum class ThreadPriority { VeryHigh };
inline void SetCurrentThreadName(const char*) {}
inline void SetCurrentThreadPriority(ThreadPriority) {}
inline void SetCurrentThreadToPerformanceCores() {}
inline void PollThreadPolicies() {}
}
""",
    "common/settings.h": """
#pragma once
namespace Settings {
struct Setting { bool value; bool GetValue() const { return value; } };
inline struct { Setting use_multi_core{true}, use_speed_limit{true}, use_video_framerate{false}; } values;
inline float SpeedLimit() { return 100.f; }
}
""",
    "core/core_timing.h": """
#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include "common/common_types.h"
namespace Core::Timing {
using Callback = std::function<std::optional<std::chrono::nanoseconds>(s64, std::chrono::nanoseconds)>;
struct EventType { Callback callback; };
inline auto CreateEvent(const char*, Callback callback) {
    return std::make_shared<EventType>(EventType{callback});
}
struct MockTiming {
    std::shared_ptr<EventType> event;
    void ScheduleLoopingEvent(std::chrono::nanoseconds, std::chrono::nanoseconds,
                              std::shared_ptr<EventType> next) { event = next; }
    void UnscheduleEvent(std::shared_ptr<EventType>) { event.reset(); }
};
}
""",
    "core/core.h": """
#pragma once
#include "core/core_timing.h"
namespace Kernel { class KernelCore {}; }
namespace Core {
class System {
    Timing::MockTiming timing;
    Kernel::KernelCore kernel;
public:
    bool multicore = true;
    bool IsMulticore() const { return multicore; }
    bool IsShuttingDown() const { return false; }
    bool GetNVDECActive() const { return false; }
    auto& CoreTiming() { return timing; }
    auto& Kernel() { return kernel; }
};
}
""",
    "core/hle/service/vi/display_list.h": """
#pragma once
#include "common/common_types.h"
namespace Service::VI {
struct Display { u64 GetId() const { return 0; } };
class DisplayList { public: template<class F> void ForEachDisplay(F callback) { Display d; callback(d); } };
}
""",
    "core/hle/service/vi/container.h": """
#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "common/common_types.h"
namespace Service::VI {
class Container {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    unsigned completed = 0;
public:
    bool ComposeOnDisplay(s32* interval, f32* scale, u64) {
        std::unique_lock lock{mutex};
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
        *interval = 1; *scale = 1.f;
        ++completed;
        condition.notify_all();
        return true;
    }
    bool WaitForComposition() {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; });
    }
    void Release() { std::lock_guard lock{mutex}; released = true; condition.notify_all(); }
    bool WaitForCompletion() {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds{2}, [&] { return completed > 0; });
    }
};
}
""",
    "core/hle/service/os/event.h": """
#pragma once
#include <atomic>
namespace Kernel { class KernelCore; }
namespace Service {
class Event {
public:
    std::atomic<unsigned> signals{0};
    void Signal(Kernel::KernelCore&) noexcept { ++signals; }
};
}
""",
}

HARNESS = r"""
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/vi/conductor.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/vblank_timing.h"
int main(int argc, char**) {
    const auto* option = std::getenv("NXBOX_VI_INDEPENDENT_VSYNC");
    const bool independent = option && std::strcmp(option, "1") == 0;
    Core::System system;
    system.multicore = argc == 1;
    Settings::values.use_multi_core.value = system.multicore;
    Service::VI::Container container;
    Service::VI::DisplayList displays;
    Service::Event event;
    Service::Event replacement;
    Service::VI::Conductor conductor{system, container, displays};
    conductor.LinkVsyncEvent(0, &event);
    auto tick = [&] {
        const auto period = system.CoreTiming().event->callback(0, std::chrono::nanoseconds{0});
        assert(period && period->count() >= 16'666'665 && period->count() <= 16'666'668);
    };
    if (!system.multicore) {
        container.Release();
        tick();
        assert(event.signals == 1);
        const auto sample = Service::VI::vblank_timing.ReadAndReset();
        assert(sample.ticks == 1 && sample.signals == 1 && sample.compositions == 1);
    } else {
        tick();
        assert(container.WaitForComposition());
        // No sleep: the compositor is known to be blocked and cannot signal the guest.
        assert(event.signals == (independent ? 1u : 0u));
        tick();
        tick();
        assert(event.signals == (independent ? 3u : 0u));
        // Removal must be safe even while the composition thread is blocked.
        conductor.UnlinkVsyncEvent(0, &event);
        conductor.LinkVsyncEvent(0, &replacement);
        tick();
        assert(event.signals == (independent ? 3u : 0u));
        assert(replacement.signals == (independent ? 1u : 0u));
        const auto sample = Service::VI::vblank_timing.ReadAndReset();
        assert(sample.ticks == 4 && sample.signals == (independent ? 4u : 0u));
        assert(sample.compositions == 0);
        container.Release();
        assert(container.WaitForCompletion());
        conductor.UnlinkVsyncEvent(0, &replacement);
        // Worker completion must not duplicate a notification in independent mode.
        if (independent) assert(replacement.signals == 1);
    }
    conductor.UnlinkVsyncEvent(0, &event);
}
"""


class VblankTests(unittest.TestCase):
    def test_profile_counts_intervals_and_separates_lateness_from_composition(self):
        compiler = shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="nxbox-vi-profile-") as directory:
            source = Path(directory) / "profile.cpp"
            executable = Path(directory) / "profile"
            source.write_text(r"""
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include "core/hle/service/vi/vblank_timing.h"
int main() {
    using namespace std::chrono_literals;
    using Service::VI::VblankTiming;
    VblankTiming timing;
    const auto epoch = VblankTiming::Clock::time_point{1s};
    timing.RecordTick(-1ns);
    timing.RecordTick(2ms);
    timing.RecordSignal(epoch);
    timing.RecordSignal(epoch + 16ms);
    timing.RecordSignal(epoch + 50ms);
    timing.RecordComposition(40ms);
    timing.RecordComposition(5ms);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                for (int interval = -1; interval <= 5; ++interval) {
                    timing.RecordSwapInterval(interval);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto sample = timing.ReadAndReset();
    assert(sample.ticks == 2 && sample.signals == 3 && sample.compositions == 2);
    assert(sample.late_ns_max == 2'000'000 && sample.signal_gap_ns_max == 34'000'000);
    assert(sample.compose_ns == 45'000'000 && sample.compose_ns_max == 40'000'000);
    for (const auto count : sample.intervals) assert(count == 400);
    auto empty = timing.ReadAndReset();
    assert(empty.ticks == 0 && empty.compose_ns == 0 && empty.intervals[3] == 0);
    timing.RecordSignal(epoch + 67ms);
    assert(timing.ReadAndReset().signal_gap_ns_max == 17'000'000);
    timing.Reset();
    timing.RecordSignal(epoch + 60s);
    assert(timing.ReadAndReset().signal_gap_ns_max == 0);
    // Exact opt-in only; non-UWP builds must ignore the timing switch entirely.
    assert(!Service::VI::IndependentVsyncEnabled());
    assert(!Service::VI::BisectEnabled("NXBOX_VI_PROFILE_INVALID"));
}
""")
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-pthread",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run(
                [str(executable)],
                env={
                    **os.environ,
                    "NXBOX_VI_INDEPENDENT_VSYNC": "1",
                    "NXBOX_VI_PROFILE_INVALID": "10",
                },
                check=True,
            )

    def test_profile_is_wired_to_accepted_buffers_and_the_five_second_report(self):
        producer = (ROOT / "src/core/hle/service/nvnflinger/buffer_queue_producer.cpp").read_text()
        reporter = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn("VI::vblank_timing.RecordSwapInterval(swap_interval);", producer)
        self.assertLess(
            producer.index("item.swap_interval = swap_interval;"),
            producer.index("VI::vblank_timing.RecordSwapInterval"),
        )
        self.assertIn('"VI_TIMING independent={}', reporter)
        self.assertLess(reporter.index("if (elapsed >= 5.0)"), reporter.index('"VI_TIMING '))
        self.assertIn("Service::VI::vblank_timing.ReadAndReset()", reporter)

    def test_vblank_continues_while_composition_is_blocked(self):
        compiler = shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="nxbox-vi-") as directory:
            temporary = Path(directory)
            for name, content in STUBS.items():
                target = temporary / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
            harness = temporary / "harness.cpp"
            harness.write_text(HARNESS)
            executable = temporary / "vblank"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-Wno-unused-parameter",
                    "-pthread",
                    "-DNXBOX_UWP",
                    "-I",
                    str(temporary),
                    "-I",
                    str(ROOT / "src"),
                    str(harness),
                    str(ROOT / "src/core/hle/service/vi/conductor.cpp"),
                    str(ROOT / "src/core/hle/service/vi/vsync_manager.cpp"),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            for switch in (None, "0", "1", "10"):
                for single_core in (False, True):
                    with self.subTest(switch=switch, single_core=single_core):
                        environment = {**os.environ, "NXBOX_THREAD_PROFILE": "1"}
                        environment.pop("NXBOX_VI_INDEPENDENT_VSYNC", None)
                        if switch is not None:
                            environment["NXBOX_VI_INDEPENDENT_VSYNC"] = switch
                        subprocess.run(
                            [str(executable)] + (["single"] if single_core else []),
                            env=environment,
                            check=True,
                            timeout=10,
                        )


if __name__ == "__main__":
    unittest.main()
