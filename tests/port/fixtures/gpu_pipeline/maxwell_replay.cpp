// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware stubs; the DrawManager declaration and method bodies are inserted
// verbatim from production by test_gpu_pipeline.py.
#include <array>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "video_core/gpu_draw_pipeline.h"
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using f32 = float;
using GPUVAddr = std::uint64_t;
#define LOG_TRACE(...)
#define MAXWELL3D_REG_INDEX(field) static_cast<u32>(offsetof(Maxwell3D::Regs, field) / 4)
namespace VideoCommon {
namespace Dirty {
constexpr unsigned IndexBuffer = 1;
}
namespace SubmissionTrace {
template <class... Args>
void Log(const char*, Args&&...) {}
} // namespace SubmissionTrace
} // namespace VideoCommon
template <unsigned Shift, unsigned Bits, class T = u32>
struct Field {
    u32 raw;
    operator T() const {
        return static_cast<T>((raw >> Shift) & ((1u << Bits) - 1));
    }
    T Value() const {
        return *this;
    }
};
struct Rasterizer {
    std::function<void(bool, u32)> on_draw;
    void Draw(bool indexed, u32 instances) {
        on_draw(indexed, instances);
    }
    void Clear(u32) {}
    void DrawIndirect() {}
};
namespace Tegra::Engines {
struct Maxwell3D {
    struct Regs {
        enum class PrimitiveTopology : u32 { Points, Lines, LineStrip, Triangles = 4, Quads = 7 };
        enum class PrimitiveTopologyControl : u32 { UseInBeginMethods, UseSeparateState };
        enum class PrimitiveTopologyOverride : u32 { None, Points, Lines, LineStrip };
        enum class IndexFormat : u32 { UnsignedInt };
        struct VertexBuffer {
            u32 first, count;
        };
        struct IndexBuffer {
            u32 first, count;
            IndexFormat format;
        };
        struct IndexBufferSmall {
            union {
                u32 raw;
                Field<0, 16> count;
                Field<16, 12> first;
                Field<28, 4, PrimitiveTopology> topology;
            };
        };
        struct Draw {
            enum class InstanceId : u32 { First, Subsequent, Unchanged };
            u32 end;
            union {
                u32 begin;
                Field<0, 16, PrimitiveTopology> topology;
                Field<26, 2, InstanceId> instance_id;
            };
        };
        struct Instance {
            union {
                u32 raw;
                Field<0, 16, PrimitiveTopology> topology;
                Field<16, 8> start;
                Field<24, 8> count;
            };
        };
        union {
            struct {
                u32 clear_surface;
                Draw draw;
                VertexBuffer vertex_buffer;
                IndexBuffer index_buffer;
                u32 index_buffer32_subsequent, index_buffer16_subsequent, index_buffer8_subsequent;
                u32 index_buffer32_first, index_buffer16_first, index_buffer8_first;
                u32 draw_inline_index;
                struct {
                    u32 even, odd;
                } inline_index_2x16;
                struct {
                    u32 index0, index1, index2, index3;
                } inline_index_4x8;
                Instance vertex_array_instance_first, vertex_array_instance_subsequent;
                struct {
                    u32 src_y0;
                } draw_texture;
                u32 global_base_instance_index, global_base_vertex_index;
                PrimitiveTopologyControl primitive_topology_control;
                PrimitiveTopologyOverride topology_override;
                struct {
                    u32 mode;
                    GPUVAddr Address() const {
                        return 0;
                    }
                } render_enable;
                u32 render_enable_override;
            };
            std::array<u32, 4096> reg_array{};
        };
    };
    // DRAW_MANAGER
    u32 engine_state{};
    Regs regs{};
    DrawManager draw_manager;
    std::array<u32, 16> state{};
    struct {
        std::bitset<256> flags;
    } dirty;
    std::map<u32, u32> replace_table;
    Rasterizer* rasterizer{};
    bool execute{true};
    bool ShouldExecute() const {
        return execute;
    }
};
// DRAW_METHODS
void Maxwell3D::DrawManager::DrawTexture(Maxwell3D&) {}
} // namespace Tegra::Engines
using Tegra::Engines::Maxwell3D;
struct Call {
    std::vector<u32> words;
    std::vector<u8> inline_indices;
    bool operator==(const Call&) const = default;
};
Call Record(const Maxwell3D& engine, bool indexed, u32 instances) {
    const auto& state = engine.draw_manager.draw_state;
    return {{indexed, instances, static_cast<u32>(state.topology), state.base_index,
             state.base_instance, state.vertex_buffer.first, state.vertex_buffer.count,
             state.index_buffer.first, state.index_buffer.count, engine.regs.reg_array[63]},
            state.inline_index_draw_indexes};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::map<std::string, u32> offsets;
    // REGISTER_NAMES
    std::ifstream input{argv[1]};
    assert(input);
    std::vector<std::pair<std::string, u32>> methods;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream record{line};
        std::string name;
        u32 value{};
        record >> name >> std::hex >> value;
        assert(record);
        methods.emplace_back(name, value);
    }
    for (unsigned capacity : {1u, 2u, 64u}) {
        VideoCommon::GpuDrawPipeline queue{capacity};
        Maxwell3D serial, frontend, consumer;
        Rasterizer direct, pipelined;
        serial.rasterizer = &direct;
        frontend.rasterizer = &pipelined;
        std::vector<Call> expected, actual;
        direct.on_draw = [&](bool indexed, u32 instances) {
            expected.push_back(Record(serial, indexed, instances));
        };
        pipelined.on_draw = [&](bool indexed, u32 instances) {
            auto snapshot = VideoCommon::CaptureDrawSnapshot(frontend);
            queue.Push([&, snapshot = std::move(snapshot), indexed, instances] {
                snapshot.Apply(consumer);
                actual.push_back(Record(consumer, indexed, instances));
            });
        };
        for (unsigned replay = 0; replay < 200; ++replay) {
            for (const auto& [name, value] : methods) {
                if (name == "barrier") {
                    queue.Drain();
                    assert(actual == expected);
                    continue;
                }
                if (name == "execute") {
                    serial.execute = frontend.execute = value != 0;
                    continue;
                }
                assert(offsets.contains(name));
                const u32 method = offsets.at(name);
                for (Maxwell3D* engine : {&serial, &frontend}) {
                    engine->regs.reg_array[method] = value;
                    engine->dirty.flags.set(method % 256);
                    engine->draw_manager.ProcessMethodCall(*engine, method, value);
                }
            }
        }
        queue.Drain();
        assert(expected == actual && !actual.empty());
        std::cout << "production Maxwell DrawManager capacity=" << capacity << ": " << actual.size()
                  << " draw calls matched\n";
    }
}
