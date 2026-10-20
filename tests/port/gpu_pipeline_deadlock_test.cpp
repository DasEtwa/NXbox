// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

#include "video_core/gpu_draw_pipeline.h"
#include "video_core/host1x/syncpoint_manager.h"

using namespace std::chrono_literals;

void Require(bool condition) {
    if (!condition) {
        std::cerr << "GPU pipeline wait-cycle regression failed\n";
        std::_Exit(1); // Do not join a deadlocked worker during failure cleanup.
    }
}

template <class Future>
void Await(Future& future) {
    Require(future.wait_for(3s) == std::future_status::ready);
    future.get();
}

void SyncpointRoundTrip(std::size_t capacity) {
    VideoCommon::GpuDrawPipeline pipeline{capacity};
    Tegra::Host1x::SyncpointManager syncpoints;
    std::promise<void> started, callback_entered, release_callback;
    auto entered = callback_entered.get_future().share();
    auto release = release_callback.get_future().share();
    std::atomic<unsigned> draws{}, presents{}, cpu_writes{};

    // This represents a builder job incrementing a syncpoint. Let another
    // thread enter a guest presentation callback before it takes guard.
    pipeline.Push([&] {
        started.set_value();
        entered.wait();
        syncpoints.IncrementGuest(1);
        ++draws;
    });
    auto started_future = started.get_future();
    Await(started_future);
    for (std::size_t i = 1; i < capacity; ++i) {
        pipeline.Push([&] { ++draws; });
    }

    syncpoints.RegisterGuestAction(0, 1, [&] {
        callback_entered.set_value();
        release.wait();
        // RendererD3D12::Composite's synchronous round trip. Before the fix
        // this callback owned guard and the older builder job needed guard:
        // Invoke (or its full-FIFO Push) could never finish.
        pipeline.Invoke([&] { ++presents; });
    });
    auto signal = std::async(std::launch::async, [&] { syncpoints.IncrementGuest(0); });
    Require(entered.wait_for(3s) == std::future_status::ready);

    // Model OnCPUWrite arriving behind the blocked job as well.
    auto cpu = std::async(std::launch::async, [&] { pipeline.Invoke([&] { ++cpu_writes; }); });
    release_callback.set_value();
    Await(signal);
    Await(cpu);
    pipeline.Drain();
    Require(draws == capacity && presents == 1 && cpu_writes == 1);
    Require(syncpoints.GetGuestSyncpointValue(0) == 1);
    Require(syncpoints.GetGuestSyncpointValue(1) == 1);

    // A detached guest callback may reenter the manager, and actions above
    // the current threshold must remain registered and execute only once.
    unsigned callbacks = 0;
    syncpoints.RegisterGuestAction(2, 2, [&] { ++callbacks; });
    syncpoints.RegisterGuestAction(2, 1, [&] {
        ++callbacks;
        syncpoints.RegisterGuestAction(2, 1, [&] { ++callbacks; });
        syncpoints.IncrementGuest(3);
    });
    syncpoints.IncrementGuest(2);
    Require(callbacks == 2);
    syncpoints.IncrementGuest(2);
    Require(callbacks == 3);
}

int main() {
    // A separate deadline also covers unexpected waits outside Await().
    std::jthread watchdog{[](std::stop_token stop) {
        for (unsigned i = 0; i < 1500 && !stop.stop_requested(); ++i)
            std::this_thread::sleep_for(10ms);
        if (!stop.stop_requested())
            std::_Exit(2);
    }};
    for (auto capacity : {1u, 2u, 64u})
        SyncpointRoundTrip(capacity);
    watchdog.request_stop();
}
