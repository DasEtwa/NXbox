// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
namespace Core {
struct System {};
} // namespace Core
namespace Engines {
enum class EngineTypes { Maxwell3D, Other };
struct Engine {
    std::vector<std::pair<u32, u32>> events;
    u64 current_dma_segment{};
    void ConsumeSink(Core::System&) {}
};
struct Maxwell3D : Engine {
    struct Regs {
        u32 padding[64];
        struct {
            u32 buffer[16];
        } const_buffer;
    };
    unsigned batches{};
    void CallCBDataIncrementing(u32 first, const u32* values, u32 count) {
        ++batches;
        for (u32 i = 0; i < count; ++i)
            events.emplace_back(first + i, values[i]);
    }
};
} // namespace Engines
enum class SubmissionMode { Increasing, NonIncreasing, Inline, IncreaseOnce };
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
struct DmaPusher {
    struct State {
        u32 method{}, method_count{}, subchannel{};
        u64 dma_word_offset{}, dma_get{100};
        bool non_incrementing{}, is_last_call{};
    } dma_state;
    bool dma_increment_once{};
    Core::System system;
    Engines::Maxwell3D engine;
    std::array<Engines::Engine*, 1> subchannels{&engine};
    std::array<Engines::EngineTypes, 1> subchannel_type{Engines::EngineTypes::Maxwell3D};
    void SetState(const CommandHeader& h) {
        dma_state.method = h.method;
        dma_state.method_count = h.method_count;
        dma_state.subchannel = h.subchannel;
    }
    void CallMethod(u32 value) {
        engine.events.emplace_back(dma_state.method, value);
    }
    void CallMultiMethod(const u32* values, u32 count) {
        for (u32 i = 0; i < count; ++i)
            CallMethod(values[i]);
    }
    void ProcessCommands(std::span<const CommandHeader> commands);
};
struct ReferencePusher : DmaPusher {
    void ProcessCommands(std::span<const CommandHeader> commands);
};
// METHODS
int main() {
    // Start at each CB_DATA register, cross its end, and split submissions at every
    // word. The scalar parser and batched parser must emit the same method stream.
    for (u32 first = 64; first < 80; ++first) {
        for (u32 split = 0; split <= 20; ++split) {
            for (auto type : {Engines::EngineTypes::Maxwell3D, Engines::EngineTypes::Other}) {
                for (auto mode : {SubmissionMode::Increasing, SubmissionMode::IncreaseOnce,
                                  SubmissionMode::NonIncreasing}) {
                    DmaPusher batch;
                    ReferencePusher scalar;
                    batch.subchannel_type[0] = scalar.subchannel_type[0] = type;
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
