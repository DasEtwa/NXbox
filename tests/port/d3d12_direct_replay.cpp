// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_record_args.h"
#include "video_core/renderer_d3d12/d3d12_recorded_state.h"

thread_local bool track_allocations{};
thread_local unsigned allocations{};
void* operator new(size_t size) {
    if (track_allocations)
        ++allocations;
    if (void* value = std::malloc(size ? size : 1))
        return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept {
    std::free(value);
}
using namespace D3D12;

struct State {
    uint64_t checksum{}, calls{}, draws{};
    void Add(uint64_t value) {
        checksum = (checksum ^ value) * 1099511628211ull;
        ++calls;
    }
};
struct Draw {
    uint32_t index;
    void Replay(void* target) const {
        auto& state = *static_cast<State*>(target);
        state.Add(index);
        ++state.draws;
    }
};
struct Lease {
    std::atomic<unsigned> refs{1};
    void AddRef() {
        ++refs;
    }
    void Release() {
        --refs;
    }
};

void Correctness(bool fast) {
    RecordStream<> stream{RecordStream<>::chunk_count, fast};
    uint32_t values[16];
    for (unsigned i = 0; i < 16; ++i)
        values[i] = i;
    EncodeCall<[](void* target, const uint32_t* data) {
        assert(reinterpret_cast<uintptr_t>(data) % 16 == 0);
        for (unsigned i = 0; i < 16; ++i)
            assert(data[i] == i);
        static_cast<State*>(target)->Add(data[15]);
    }>(stream, Snapshot(values, 16));
    for (unsigned i = 0; i < 40; ++i)
        stream.Repeat(Draw{i});
    EncodeCall<[](void* target, const uint32_t* data) {
        assert(reinterpret_cast<uintptr_t>(data) % 16 == 0);
        static_cast<State*>(target)->Add(data[0]);
    }>(stream, Snapshot(values, 16));
    stream.Close();
    State state;
    allocations = 0;
    track_allocations = true;
    assert(stream.Consume(&state));
    track_allocations = false;
    // This assertion fails on the old transport: its first replay allocates scratch.
    assert(fast ? allocations == 0 : allocations > 0);
    allocations = 0;
    track_allocations = true;
    assert(stream.Consume(&state));
    assert(stream.Consume(&state));
    track_allocations = false;
    assert(!fast || allocations == 0);
    assert(!stream.Consume(&state) && state.calls == 42 && state.draws == 40);

    // Tiny, non-aligned chunks force header and payload splits, ring wrap, oversized
    // snapshots and compact packets. Check exact order with simultaneous production.
    RecordStream<127, 2> tiny{2, fast};
    State actual, expected;
    Lease lease;
    std::thread worker([&] {
        while (tiny.Consume(&actual)) {
        }
    });
    for (unsigned i = 0; i < 5000; ++i) {
        uint32_t data[80];
        for (unsigned j = 0; j < 80; ++j)
            data[j] = i + j;
        EncodeCall<[](void* target, const uint32_t* data, Lease*) {
            assert(reinterpret_cast<uintptr_t>(data) % 16 == 0);
            static_cast<State*>(target)->Add(data[0] + data[79]);
        }>(tiny, Snapshot(data, 80), &lease);
        expected.Add(i + i + 79);
        tiny.Repeat(Draw{i});
        Draw{i}.Replay(&expected);
        if (i % 29 == 0)
            tiny.Flush();
    }
    tiny.Close();
    worker.join();
    assert(actual.checksum == expected.checksum && actual.calls == expected.calls);
    assert(lease.refs == 1);

    // A throw in the last record of a chunk must retire that chunk, drain later
    // holds exactly once, and unblock a producer trying to recycle it.
    RecordStream<128, 2> failure{2, fast};
    std::thread drain([&] {
        bool execute = true;
        for (;;) {
            try {
                if (!failure.Consume(nullptr, execute))
                    break;
            } catch (const std::runtime_error&) {
                execute = false;
            }
        }
    });
    EncodeCall<[](void*, Lease*, const uint32_t*) {
        throw std::runtime_error("injected replay failure");
    }>(failure, &lease, Snapshot(values, 16));
    for (unsigned i = 0; i < 5000; ++i)
        EncodeCall<[](void*, Lease*) { assert(false); }>(failure, &lease);
    failure.Close();
    drain.join();
    assert(lease.refs == 1);
    std::cout << (fast ? "direct" : "scratch")
              << ": alignment, allocation, split/recycle/order, exception/hold drain OK\n";
}

struct List : State {
    struct Handle {
        uint64_t ptr;
    };
    void SetGraphicsRootDescriptorTable(uint32_t slot, Handle handle) {
        Add(slot ^ handle.ptr);
    }
    void SetGraphicsRootConstantBufferView(uint32_t slot, uint64_t address) {
        Add(slot ^ address);
    }
};
struct Target {
    List* list;
};
struct CompactDraw {
    uint32_t count, instances, first, first_instance;
    int32_t base_vertex;
    uint32_t indexed;
    void Replay(void* target) const {
        auto& list = *static_cast<Target*>(target)->list;
        list.Add(first);
        list.Add(count);
        ++list.draws;
    }
};
void EncodeFrame(RecordStream<>& stream, bool compact) {
    for (unsigned draw = 0; draw < 3580; ++draw) {
        RecordedRootBinding roots[8];
        for (unsigned i = 0; i < 8; ++i)
            roots[i] = {uint64_t(draw * 256 + i * 16), i, i >= 5};
        RecordGraphicsRootBindings<Target>(stream, roots, 8);
        uint32_t constants[16];
        for (unsigned i = 0; i < 16; ++i)
            constants[i] = draw + i;
        EncodeCall<[](void* target, const uint32_t* words) {
            auto& list = *static_cast<Target*>(target)->list;
            for (unsigned i = 0; i < 16; ++i)
                list.Add(words[i]);
        }>(stream, Snapshot(constants, 16));
        EncodeCall<[](void* target, uint64_t pipeline, uint32_t topology) {
            auto& list = *static_cast<Target*>(target)->list;
            list.Add(pipeline);
            list.Add(topology);
        }>(stream, uint64_t{draw % 73}, uint32_t{3});
        if (compact) {
            stream.Repeat(CompactDraw{96, 1, draw, 0, 0, 1});
            continue;
        }
        EncodeCall<[](void* target, uint32_t index, uint32_t count) {
            auto& list = *static_cast<Target*>(target)->list;
            list.Add(index);
            list.Add(count);
            ++list.draws;
        }>(stream, draw, uint32_t{96});
    }
    stream.Flush();
}

void Benchmark() {
    uint64_t reference{};
    for (bool compact : {false, true}) {
        for (bool fast : {false, true}) {
            RecordStream<> stream{RecordStream<>::chunk_count, fast};
            List list;
            Target target{&list};
            constexpr unsigned frames = 120, warmup = 10;
            std::vector<double> replay_samples, encode_samples;
            replay_samples.reserve(frames);
            encode_samples.reserve(frames);
            for (unsigned frame = 0; frame < frames + warmup; ++frame) {
                const auto encode_start = std::chrono::steady_clock::now();
                EncodeFrame(stream, compact);
                const auto start = std::chrono::steady_clock::now();
                for (unsigned command = 0; command < 3580 * 4; ++command)
                    assert(stream.Consume(&target));
                const auto end = std::chrono::steady_clock::now();
                if (frame >= warmup) {
                    replay_samples.push_back(
                        std::chrono::duration<double, std::milli>(end - start).count());
                    encode_samples.push_back(
                        std::chrono::duration<double, std::milli>(start - encode_start).count());
                }
            }
            stream.Close();
            assert(!stream.Consume(&target));
            assert(list.draws == 3580 * (frames + warmup));
            if (!fast)
                reference = list.checksum;
            assert(list.checksum == reference);
            std::sort(replay_samples.begin(), replay_samples.end());
            std::sort(encode_samples.begin(), encode_samples.end());
            std::cout << (fast ? "direct" : "scratch") << (compact ? " compact" : " encoded")
                      << " 3580 draws/frame, 14320 packets/frame median: replay="
                      << replay_samples[frames / 2] << " encode=" << encode_samples[frames / 2]
                      << " ms/frame checksum=" << list.checksum << '\n';
        }
    }
}
int main(int argc, char**) {
    if (argc > 1) {
        Benchmark();
        return 0;
    }
    Correctness(false);
    Correctness(true);
}
