// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include "common/gpu_thread_profile.h"
#include "video_core/gpu_draw_pipeline.h"

// Hardware-free rasterizer contract. Production snapshot/queue code owns these
// fields exactly as it owns Maxwell's registers, dirty flags, draw manager and
// CBs.
struct Engine {
    unsigned engine_state{};
    std::array<unsigned, 4096> regs{};
    std::array<unsigned, 16> state{};
    struct Draw {
        unsigned first{}, count{}, instances{1};
        std::vector<unsigned> inline_indices;
    } draw_manager;
    struct Dirty {
        std::bitset<256> flags;
    } dirty;
    std::map<unsigned, unsigned> replace_table;
};
struct Call {
    unsigned first, count, instances, state, uniform, memory, replacement;
    std::vector<unsigned> indices;
    bool operator==(const Call&) const = default;
};
Call Draw(Engine& engine, unsigned memory) {
    const auto& params = engine.draw_manager;
    Call result{params.first,    params.count, params.instances,        engine.regs[4],
                engine.state[0], memory,       engine.replace_table[0], params.inline_indices};
    engine.dirty.flags.reset(); // Caches consume flags only on the consumer.
    return result;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream stream{argv[1]};
    assert(stream);
    std::string record;
    std::vector<std::array<unsigned, 2>> methods;
    while (std::getline(stream, record)) {
        if (record.empty() || record[0] == '#')
            continue;
        std::istringstream words{record};
        unsigned method{}, argument{};
        words >> std::hex >> method >> argument;
        assert(words);
        methods.push_back({method, argument});
    }
    assert(!methods.empty());
    for (std::size_t capacity : {1u, 2u, 64u}) {
        VideoCommon::GpuDrawPipeline queue{capacity};
        Engine serial, frontend, consumer;
        std::vector<Call> expected, actual;
        unsigned memory = 7;
        // Replay the immutable method recording repeatedly across bounded queue
        // wraps.
        for (unsigned replay = 0; replay < 300; ++replay) {
            for (const auto& [method, value] : methods) {
                if (method == 0x300) { // draw trigger
                    expected.push_back(Draw(serial, memory));
                    auto snapshot = VideoCommon::CaptureDrawSnapshot(frontend);
                    queue.Push([&, snapshot = std::move(snapshot)] {
                        assert(queue.IsConsumer());
                        snapshot.Apply(consumer);
                        actual.push_back(Draw(consumer, memory));
                    });
                } else if (method >= 0x400) {
                    // Queries, semaphores, uploads, copies and flushes are ordering
                    // points.
                    queue.Drain();
                    assert(actual == expected);
                    if (method == 0x400)
                        memory = value;
                    queue.Invoke([&] {
                        queue.Drain(); // Consumer reentrancy must not wait for itself.
                        assert(actual == expected);
                    });
                } else {
                    for (Engine* engine : {&serial, &frontend}) {
                        if (method == 0x200)
                            engine->draw_manager.first = value;
                        else if (method == 0x201)
                            engine->draw_manager.count = value;
                        else if (method == 0x202)
                            engine->draw_manager.instances = value;
                        else if (method == 0x203)
                            engine->draw_manager.inline_indices.push_back(value);
                        else if (method == 0x204)
                            engine->draw_manager.inline_indices.clear();
                        else if (method == 0x205)
                            engine->state[0] = value;
                        else if (method == 0x206)
                            engine->replace_table[0] = value;
                        else {
                            engine->regs[method] = value;
                            engine->dirty.flags.set(method % 256);
                        }
                    }
                }
            }
        }
        queue.Drain();
        assert(actual == expected);
        assert(queue.Invoke([] { return 42; }) == 42);
        int object = 0;
        int& reference = queue.Invoke([&]() -> int& { return object; });
        reference = 9;
        assert(object == 9);
        try {
            queue.Invoke([] { throw std::runtime_error("synchronous failure"); });
            assert(false);
        } catch (const std::runtime_error&) {
        }
        queue.Invoke([] {}); // A synchronous failure must not strand the queue.
        std::cout << "capacity=" << capacity << ": " << actual.size()
                  << " replay draws matched; synchronous barriers matched\n";
    }
    Engine source, destination;
    source.engine_state = 1;
    source.dirty.flags.set(3);
    destination.dirty.flags.set(9); // Cache-generated invalidation must survive.
    auto snapshot = VideoCommon::CaptureDrawSnapshot(source);
    source.regs[4] = 99;
    source.draw_manager.inline_indices.push_back(99);
    assert(source.dirty.flags.none());
    snapshot.Apply(destination);
    assert(destination.engine_state == 1);
    assert(destination.dirty.flags[3] && destination.dirty.flags[9]);
    assert(destination.regs[4] == 0 && destination.draw_manager.inline_indices.empty());

    // Deterministic saturation: the second publisher cannot enter a capacity-one
    // queue.
    VideoCommon::GpuDrawPipeline bounded{1};
    std::promise<void> started, release, publishing;
    auto gate = release.get_future().share();
    bounded.Push([&] {
        started.set_value();
        gate.wait();
    });
    started.get_future().wait();
    std::atomic<bool> entered{};
    auto producer = std::async(std::launch::async, [&] {
        publishing.set_value();
        bounded.Push([&] { entered = true; });
    });
    publishing.get_future().wait();
    assert(producer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(!entered);
    release.set_value();
    producer.get();
    bounded.Drain();
    assert(entered);
    bounded.Close();
    bounded.Close(); // Teardown is idempotent and publication is rejected afterwards.
    try {
        bounded.Push([] {});
        assert(false);
    } catch (const std::runtime_error&) {
    }

    std::atomic<unsigned> completed{};
    {
        VideoCommon::GpuDrawPipeline closing{2};
        for (unsigned i = 0; i < 30; ++i)
            closing.Push([&] { ++completed; });
    }
    assert(completed == 30); // Destruction joins after draining owned snapshots.
    VideoCommon::GpuDrawPipeline failed{1};
    failed.Push([] { throw std::runtime_error("draw failure"); });
    try {
        failed.Drain();
        assert(false);
    } catch (const std::runtime_error&) {
    }
    try {
        failed.Push([] {});
        assert(false);
    } catch (const std::runtime_error&) {
    }
    VideoCommon::GpuDrawPipeline cancellation{2};
    std::promise<void> failing_started, release_failure, invoke_started;
    auto failure_gate = release_failure.get_future().share();
    cancellation.Push([&] {
        failing_started.set_value();
        failure_gate.wait();
        throw std::runtime_error("cancel following synchronous task");
    });
    failing_started.get_future().wait();
    auto cancelled = std::async(std::launch::async, [&] {
        invoke_started.set_value();
        try {
            cancellation.Invoke([] { assert(false); });
        } catch (const std::exception&) {
            return;
        }
        assert(false);
    });
    invoke_started.get_future().wait();
    assert(cancelled.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    release_failure.set_value();
    assert(cancelled.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    cancelled.get();
    Common::GpuProfile::recording_enabled = true;
    VideoCommon::GpuDrawPipeline profiling;
    profiling.Invoke([] { assert(!Common::GpuProfile::recording_enabled); });
    std::cout << "snapshot ownership, dirty union, saturation, shutdown, "
                 "exceptions and TLS OK\n";
}
