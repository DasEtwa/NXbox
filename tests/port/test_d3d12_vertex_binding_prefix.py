# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare trimmed production IA bindings with the legacy 32-slot draw path."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class VertexBindingPrefixTests(unittest.TestCase):
    def test_production_draw_uses_prefix_with_bisect_and_trace_fallbacks(self):
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        draw = source.split("void RasterizerD3D12::PrepareDraw(", 1)[1].split(
            "void RasterizerD3D12::Draw(", 1
        )[0]
        self.assertIn('!BisectEnabled("NXBOX_D3D12_NO_VERTEX_BINDING_PREFIX")', draw)
        self.assertIn("vertex_binding_prefix && !draw_trace_enabled", draw)
        self.assertIn("Pso::VertexBufferBindingCount(native_pipeline->State())", draw)
        self.assertIn("for (u32 i = 0; i < num_vertex_views; ++i)", draw)
        self.assertIn("recorded.vertices.Update(vertex_views.data(), num_vertex_views)", draw)
        self.assertIn(
            "commands->IASetVertexBuffers(0, num_vertex_views, vertex_views.data())", draw
        )
        loop = draw.split("for (u32 i = 0; i < num_vertex_views; ++i)", 1)[1]
        self.assertIn("vertex_views[i] = {zero_vertex_address, 65536, 0}", loop)
        self.assertEqual(draw.count("zero_vertex_buffer->handle->GetGPUVirtualAddress()"), 1)
        # The native PSO uses attribute indices as IA slots, not guest stream slots.
        pipeline = (NATIVE / "d3d12_graphics_pipeline.cpp").read_text()
        self.assertRegex(pipeline, r"static_cast<DXGI_FORMAT>\(a.dxgi\),\s+i,\s+0,")

    def test_native_layout_replay_and_stream_size(self):
        program = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_recorded_state.h"
using namespace D3D12;
using namespace D3D12::Pso;
struct View {
    std::uint64_t address;
    unsigned size, stride;
};
struct List {
    std::array<View, 32> views{};
    void Bind(unsigned start, unsigned count, const View* next) {
        assert(start + count <= views.size());
        std::memcpy(views.data() + start, next, count * sizeof(View));
    }
};
struct SizeStream {
    size_t bytes{};
    template <class Command> void Begin(size_t extra) {
        bytes += sizeof(RecordStream<>::Header) + sizeof(Command) + extra;
    }
    void Write(const void*, size_t) {}
};
int main() {
    GraphicsPsoState edge{};
    assert(VertexBufferBindingCount(edge) == 0);
    edge.attributes[31] = {1, 0, 0, 2, 0, 0, 0, 0, 0};
    assert(VertexBufferBindingCount(edge) == 32);
    edge.attributes[31].dxgi = 0; // Native input-layout builder skips unknown formats.
    assert(VertexBufferBindingCount(edge) == 0);
    ApplyVertexReads(edge, (1u << 30) | 1u);
    assert(edge.attributes[30].enabled == 2);
    assert(VertexBufferBindingCount(edge) == 31); // Zero-reading inputs still need IA views.
    RecordStream<79, 2> stream; // Split snapshots and commands across chunk boundaries.
    List expected, replay;
    RecordedArray<View, 32> recorded;
    std::thread consumer([&] { while (stream.Consume(&replay)) {} });
    const auto bind = [](void* target, unsigned count, const View* values) {
        static_cast<List*>(target)->Bind(0, count, values);
    };
    const auto check = [](void* target, std::uint32_t reads,
                          const View* oracle) {
        const auto& actual = static_cast<List*>(target)->views;
        for (unsigned i = 0; i < 32; ++i)
            if (reads & (1u << i))
                assert(std::memcmp(&actual[i], &oracle[i], sizeof(View)) == 0);
    };
    std::mt19937 random{20261019};
    for (unsigned draw = 0; draw < 6000; ++draw) {
        GraphicsPsoState state{};
        // Include growth, shrink, holes, all-zero inputs and slot 31.
        const unsigned width = draw % 33;
        const std::uint32_t mask = width == 32 ? 0xffffffffu : (1u << width) - 1u;
        const std::uint32_t reads = random() & mask;
        for (unsigned i = 0; i < 32; ++i)
            if (random() % 3)
                state.attributes[i] = {1, static_cast<std::uint8_t>(random() % 32),
                                       0, 2, 0, 0, 0, 0, 0};
        ApplyVertexReads(state, reads);
        const auto count = VertexBufferBindingCount(state);
        std::array<View, 32> views{};
        for (unsigned i = 0; i < 32; ++i) {
            views[i] = {0x10000, 65536, 0};
            if (state.attributes[i].enabled == 1)
                views[i] = {0x100000000ull + random(), 4096, 16};
            if (reads & (1u << i)) assert(i < count);
        }
        expected.Bind(0, 32, views.data());
        if (draw % 19 == 0) recorded.valid = false; // New list or helper invalidation.
        if (recorded.Update(views.data(), count))
            EncodeCall<bind>(stream, static_cast<unsigned>(count), Snapshot(views.data(), count));
        EncodeCall<check>(stream, reads, Snapshot(expected.views.data(), expected.views.size()));
        views = {}; // Producer scratch must not affect replay.
    }
    stream.Close();
    consumer.join();
    std::array<View, 32> typical{};
    SizeStream full, prefix;
    EncodeCall<bind>(full, 32u, Snapshot(typical.data(), 32));
    EncodeCall<bind>(prefix, 4u, Snapshot(typical.data(), 4));
    assert(prefix.bytes < full.bytes / 4);
    std::cout << "6000 IA replay draws OK; 4-slot encoded bytes=" << prefix.bytes
              << " legacy=" << full.bytes << '\n';
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-vertex-prefix-") as directory:
            source = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            source.write_text(program)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-pthread",
                    "-fsanitize=address,undefined",
                    f"-I{ROOT / 'src'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
