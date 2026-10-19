// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <vector>
#include "video_core/gpu_frontend.h"
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
constexpr u32 MacroRegistersStart = 0xE00;
namespace Core {
struct System {};
} // namespace Core
namespace Engines {
enum class EngineTypes { Maxwell3D, Other };
struct Engine {
    std::vector<std::pair<u32, u32>> events;
    u64 current_dma_segment{};
    std::bitset<65535> execution_mask;
    std::vector<std::pair<u32, u32>> method_sink;
    std::vector<std::tuple<u32, u32, bool, u64>> effects;
    void ConsumeSink(Core::System&) {
        events.insert(events.end(), method_sink.begin(), method_sink.end());
        method_sink.clear();
    }
    void CallMethod(Core::System&, u32 method, u32 value, bool last) {
        events.emplace_back(method, value);
        if (method < 64 || method >= 80)
            effects.emplace_back(method, value, last, current_dma_segment);
    }
    void CallMultiMethod(Core::System& system, u32 method, const u32* values, u32 count,
                         u32 pending) {
        for (u32 i = 0; i < count; ++i)
            CallMethod(system, method, values[i], pending - i <= 1);
    }
};
struct Maxwell3D : Engine {
    struct Regs {
        u32 padding[64];
        struct {
            u32 buffer[16];
        } const_buffer;
    };
    unsigned batches{}, increasing_batches{};
    void CallMultiMethod(Core::System&, u32 first, const u32* values, u32 count) {
        ++increasing_batches;
        for (u32 i = 0; i < count; ++i)
            method_sink.emplace_back(first + i, values[i]);
    }
    void CallCBDataIncrementing(u32 first, const u32* values, u32 count) {
        ++batches;
        for (u32 i = 0; i < count; ++i)
            events.emplace_back(first + i, values[i]);
    }
};
} // namespace Engines
enum class SubmissionMode { Increasing = 1, NonIncreasing = 3, Inline = 4, IncreaseOnce = 5 };
template <unsigned Shift, unsigned Bits, typename T = u32>
struct Field {
    u32 raw;
    operator T() const {
        return static_cast<T>((raw >> Shift) & ((1u << Bits) - 1));
    }
    Field& operator=(T value) {
        constexpr u32 mask = ((1u << Bits) - 1) << Shift;
        raw = (raw & ~mask) | (static_cast<u32>(value) << Shift);
        return *this;
    }
};
union CommandHeader {
    u32 argument;
    Field<0, 13> method;
    Field<13, 3> subchannel;
    Field<16, 13> method_count;
    Field<16, 13> arg_count;
    Field<29, 3, SubmissionMode> mode;
};
static_assert(sizeof(CommandHeader) == sizeof(u32));
struct DmaPusher;
namespace Engines {
struct Puller {
    struct MethodCall {
        u32 method, argument, subchannel, pending;
    };
    void CallPullerMethod(DmaPusher&, MethodCall);
    void CallMultiMethod(DmaPusher&, u32, u32, const u32*, u32, u32);
};
} // namespace Engines
struct DmaPusher {
    struct State {
        u32 method{}, method_count{}, subchannel{};
        u64 dma_word_offset{}, dma_get{100};
        bool non_incrementing{}, is_last_call{};
    } dma_state;
    static constexpr u32 non_puller_methods = 0x40;
    bool frontend_fastpath{Tegra::GpuFrontendFastPathEnabled()};
    bool dma_increment_once{};
    Core::System system;
    Engines::Maxwell3D engine;  // subchannel 0
    Engines::Maxwell3D engine1; // subchannel 1
    Engines::Engine engine2;    // subchannel 2 (not Maxwell3D)
    std::array<Engines::Engine*, 3> subchannels{&engine, &engine1, &engine2};
    std::array<Engines::EngineTypes, 3> subchannel_type{Engines::EngineTypes::Maxwell3D,
                                                        Engines::EngineTypes::Maxwell3D,
                                                        Engines::EngineTypes::Other};
    void SetState(const CommandHeader& h) {
        dma_state.method = h.method;
        dma_state.method_count = h.method_count;
        dma_state.subchannel = h.subchannel;
    }
    Engines::Puller puller;
    void CallMethod(u32 value);
    void CallMultiMethod(const u32* values, u32 count);
    void ProcessCommands(std::span<const CommandHeader> commands);
};
struct ReferencePusher : DmaPusher {
    void ProcessCommands(std::span<const CommandHeader> commands);
};
void Engines::Puller::CallPullerMethod(DmaPusher& pusher, MethodCall call) {
    pusher.engine.events.emplace_back(call.method, call.argument);
}
void Engines::Puller::CallMultiMethod(DmaPusher& pusher, u32 method, u32 subchannel,
                                      const u32* values, u32 count, u32 pending) {
    for (u32 i = 0; i < count; ++i)
        CallPullerMethod(pusher, {method, values[i], subchannel, pending - i});
}
// METHODS
bool same_engine(const Engines::Engine& a, const Engines::Engine& b) {
    return a.method_sink == b.method_sink && a.events == b.events && a.effects == b.effects &&
           a.current_dma_segment == b.current_dma_segment;
}
int main() {
    constexpr unsigned seeds = 800;
    unsigned long long compared = 0, b0 = 0, b1 = 0;
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        std::mt19937 random{seed * 2654435761u + 17};
        DmaPusher batch;
        DmaPusher scalar;
        batch.frontend_fastpath = true;
        scalar.frontend_fastpath = false;
        const unsigned dens[3] = {static_cast<unsigned>(random() % 101),
                                  static_cast<unsigned>(random() % 101),
                                  static_cast<unsigned>(random() % 101)};
        for (u32 reg = 0; reg < 65535; ++reg) {
            for (int which = 0; which < 3; ++which) {
                bool executable = reg >= MacroRegistersStart || (random() % 100) < dens[which];
                if (reg >= 64 && reg < 80)
                    executable = true;
                Engines::Engine* eb = which == 0   ? &batch.engine
                                      : which == 1 ? static_cast<Engines::Engine*>(&batch.engine1)
                                                   : &batch.engine2;
                Engines::Engine* es = which == 0   ? &scalar.engine
                                      : which == 1 ? static_cast<Engines::Engine*>(&scalar.engine1)
                                                   : &scalar.engine2;
                eb->execution_mask[reg] = es->execution_mask[reg] = executable;
            }
        }
        std::vector<CommandHeader> stream;
        for (unsigned packet = 0; packet < 400; ++packet) {
            CommandHeader header{};
            const u32 count = (random() % 4 == 0) ? 1 + random() % 200 : 1 + random() % 6;
            header.method = random() % 5 < 4 ? 0x30 + random() % (MacroRegistersStart + 8 - 0x30)
                                             : random() % 0x2000;
            header.subchannel = random() % 3;
            header.method_count = count;
            constexpr std::array modes{SubmissionMode::Increasing, SubmissionMode::Increasing,
                                       SubmissionMode::Increasing, SubmissionMode::NonIncreasing,
                                       SubmissionMode::Inline,     SubmissionMode::IncreaseOnce};
            const auto mode = modes[random() % modes.size()];
            header.mode = mode;
            stream.push_back(header);
            if (mode != SubmissionMode::Inline)
                for (u32 i = 0; i < count; ++i) {
                    CommandHeader v{};
                    v.argument = random();
                    stream.push_back(v);
                }
        }
        for (size_t i = 0; i < stream.size();) {
            const auto count = std::min<size_t>(
                1 + (random() % 3 == 0 ? random() % 300 : random() % 9), stream.size() - i);
            const auto part = std::span{stream}.subspan(i, count);
            batch.dma_state.dma_get = scalar.dma_state.dma_get = 100 + i * sizeof(u32);
            batch.ProcessCommands(part);
            scalar.ProcessCommands(part);
            ++compared;
            assert(same_engine(batch.engine, scalar.engine));
            assert(same_engine(batch.engine1, scalar.engine1));
            assert(same_engine(batch.engine2, scalar.engine2));
            assert(batch.dma_state.method == scalar.dma_state.method &&
                   batch.dma_state.method_count == scalar.dma_state.method_count);
            assert(batch.dma_state.subchannel == scalar.dma_state.subchannel);
            assert(batch.dma_state.non_incrementing == scalar.dma_state.non_incrementing &&
                   batch.dma_increment_once == scalar.dma_increment_once);
            assert(batch.dma_state.is_last_call == scalar.dma_state.is_last_call &&
                   batch.dma_state.dma_word_offset == scalar.dma_state.dma_word_offset);
            i += count;
        }
        assert(scalar.engine.increasing_batches == 0 &&
               scalar.engine1.increasing_batches == 0); // hatch off => no batching
        b0 += batch.engine.increasing_batches;
        b1 += batch.engine1.increasing_batches;
    }
    std::printf("seeds=%u segments=%llu fast_batches(sub0)=%llu "
                "fast_batches(sub1)=%llu OK\n",
                seeds, compared, b0, b1);
    // Start at each CB_DATA register, cross its end, and split submissions at
    // every word. The scalar parser and batched parser must emit the same method
    // stream.
    for (u32 first = 64; first < 80; ++first) {
        for (u32 split = 0; split <= 20; ++split) {
            for (auto type : {Engines::EngineTypes::Maxwell3D, Engines::EngineTypes::Other}) {
                for (auto mode : {SubmissionMode::Increasing, SubmissionMode::IncreaseOnce,
                                  SubmissionMode::NonIncreasing}) {
                    DmaPusher batch;
                    ReferencePusher scalar;
                    batch.subchannel_type[0] = scalar.subchannel_type[0] = type;
                    batch.engine.execution_mask.set();
                    scalar.engine.execution_mask.set();
                    CommandHeader header{};
                    header.method = first;
                    header.method_count = 20;
                    header.mode = mode;
                    batch.ProcessCommands(std::span{&header, 1});
                    scalar.ProcessCommands(std::span{&header, 1});
                    std::array<CommandHeader, 20> words{};
                    for (u32 i = 0; i < words.size(); ++i)
                        words[i].argument = i + 10;
                    for (auto part :
                         {std::span{words}.first(split), std::span{words}.subspan(split)}) {
                        batch.ProcessCommands(part);
                        scalar.ProcessCommands(part);
                        assert(batch.engine.events == scalar.engine.events);
                        assert(batch.dma_state.method == scalar.dma_state.method);
                        assert(batch.dma_state.method_count == scalar.dma_state.method_count);
                        assert(batch.dma_state.non_incrementing ==
                               scalar.dma_state.non_incrementing);
                        assert(batch.dma_state.is_last_call == scalar.dma_state.is_last_call);
                    }
                    if (mode == SubmissionMode::Increasing &&
                        type == Engines::EngineTypes::Maxwell3D)
                        assert(batch.engine.batches > 0);
                    else
                        assert(batch.engine.batches == 0);
                }
            }
        }
    }
}
