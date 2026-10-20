# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production draw-state invalidation against a full-reset reference."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class RecordedResetTests(unittest.TestCase):
    def test_production_state_replay(self):
        header = (NATIVE / "d3d12_recorded_state.h").read_text()
        # Compile the actual Windows-only state with SDK value types replaced by
        # fixed-size stand-ins. No D3D calls are needed to test cache decisions.
        state = header.split("struct RecordedDrawState {", 1)[1].split("} // namespace D3D12", 1)[0]
        driver = (
            r"""
#include <cassert>
#include <iostream>
#include "video_core/renderer_d3d12/d3d12_recorded_state.h"
struct ID3D12DescriptorHeap {};
struct ID3D12RootSignature {};
struct ID3D12PipelineState {};
using D3D12_GPU_VIRTUAL_ADDRESS = std::uint64_t;
using D3D_PRIMITIVE_TOPOLOGY = unsigned;
using UINT = unsigned;
using D3D12_VIEWPORT = std::array<float, 6>;
using D3D12_RECT = std::array<int, 4>;
using D3D12_VERTEX_BUFFER_VIEW = std::array<std::uint64_t, 2>;
using D3D12_INDEX_BUFFER_VIEW = D3D12_VERTEX_BUFFER_VIEW;
using D3D12_STREAM_OUTPUT_BUFFER_VIEW = std::array<std::uint64_t, 3>;
constexpr unsigned D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE = 16;
constexpr unsigned D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT = 32;
namespace D3D12 {
struct RecordedDrawState {
"""
            + state
            + r"""
}
int main() {
    using namespace D3D12;
    RecordedDrawState fast, reference;
    std::array<RecordedValue<std::uint64_t>, 64> tables, cbvs;
    ID3D12RootSignature signature;
    for (std::uint64_t iteration = 1; iteration < 10000; ++iteration) {
        const auto generation = iteration / 7 + 1;
        fast.Refresh(generation, iteration % 2 == 0);
        if (reference.generation != generation) {
            reference = {};
            reference.generation = generation;
            tables = {};
            cbvs = {};
        }
        const auto compare = [](auto& cached, auto& expected, const auto& next) {
            assert(cached.Update(next) == expected.Update(next));
            assert(!cached.Update(next));
        };
        compare(fast.heaps, reference.heaps, std::array<ID3D12DescriptorHeap*, 2>{});
        compare(fast.root_signature, reference.root_signature, &signature);
        compare(fast.pipeline, reference.pipeline, static_cast<ID3D12PipelineState*>(nullptr));
        compare(fast.topology, reference.topology, static_cast<unsigned>(iteration % 3));
        compare(fast.targets, reference.targets, std::array<std::uint64_t, 4>{iteration / 11});
        compare(fast.index, reference.index, D3D12_INDEX_BUFFER_VIEW{iteration / 13});
        compare(fast.stencil, reference.stencil, std::array<UINT, 3>{});
        compare(fast.blend, reference.blend, std::array<float, 4>{-0.0f});
        compare(fast.depth_bounds, reference.depth_bounds, std::array<float, 2>{});
        const auto arrays = [](auto& cached, auto& expected) {
            const auto values = cached.values;
            for (const auto count : {values.size(), size_t{0}, size_t{1}}) {
                assert(cached.Update(values.data(), count) == expected.Update(values.data(), count));
                assert(!cached.Update(values.data(), count));
            }
            assert(cached.Update(values.data(), values.size() + 1));
            assert(expected.Update(values.data(), values.size() + 1));
        };
        arrays(fast.viewports, reference.viewports);
        arrays(fast.scissors, reference.scissors);
        arrays(fast.vertices, reference.vertices);
        arrays(fast.stream_output, reference.stream_output);
        for (size_t slot = 0; slot < 64; ++slot) {
            const auto next = iteration / (slot + 1);
            const auto check = [&](auto& cached, auto& expected, auto& original) {
                const bool changed = original[slot].Update(next);
                assert(cached.Update(slot, next) == changed);
                assert(expected.Update(slot, next) == changed);
                assert(!cached.Update(slot, next));
            };
            check(fast.root_args.tables, reference.root_args.tables, tables);
            check(fast.root_args.cbvs, reference.root_args.cbvs, cbvs);
        }
        arrays(fast.root_args.constants, reference.root_args.constants);
        if (iteration % 3 == 0) {
            fast.ClearRootArguments();
            reference.root_args = {};
            tables = {};
            cbvs = {};
        }
    }
    // Root-signature/heap changes invalidate roots without touching other draw state.
    fast.blend.Update({1, 2, 3, 4});
    fast.root_args.tables.Update(63, 1234);
    fast.root_args.cbvs.Update(0, 5678);
    const std::array<std::uint32_t, 2> constants{12, 34};
    fast.root_args.constants.Update(constants.data(), constants.size());
    fast.ClearRootArguments();
    assert(!fast.blend.Update({1, 2, 3, 4}));
    assert(fast.root_args.tables.Update(63, 1234));
    assert(fast.root_args.cbvs.Update(0, 5678));
    assert(fast.root_args.constants.Update(constants.data(), constants.size()));
    assert(!fast.root_args.constants.Update(constants.data(), constants.size()));
    // Invalidation only changes validity, never the stored payload or array count.
    fast.blend.Update({1, 2, 3, 4});
    fast.root_args.tables.Update(63, 0x1234);
    const auto blend = fast.blend.value;
    const auto table = fast.root_args.tables.values[63];
    fast.Refresh(fast.generation + 1);
    assert(fast.blend.value == blend);
    assert(fast.root_args.tables.values[63] == table);
    assert(fast.blend.Update(blend));
    fast.Refresh(fast.generation + 1, true);
    assert((fast.blend.value == std::array<float, 4>{}));
    assert(fast.blend.Update(blend));
    assert(fast.root_args.tables.Update(63, table));
    assert(fast.root_args.tables.Update(64, table)); // Never shift by 64 or index out of bounds.
    assert(fast.root_args.tables.Update(63, table));
    RecordedSlots<float, 64> floats;
    RecordedValue<float> scalar;
    const std::array<std::uint32_t, 6> patterns{0, 0x80000000, 0x7fc00001,
                                               0x7fc00002, 0x3f800000, 0x7fc00002};
    for (const auto pattern : patterns) {
        float value;
        std::memcpy(&value, &pattern, sizeof(value));
        assert(floats.Update(63, value) == scalar.Update(value));
        assert(!floats.Update(63, value));
    }
    assert(sizeof(fast.root_args) < 1200);
    std::cout << "9999 draw-state replays matched full reset; payload preserved\n";
}
"""
        )
        # This first program also compiles with the pre-change header: its
        # payload assertion fails at runtime rather than on the new slot API.
        payload_driver = (
            driver.split("int main()", 1)[0]
            + r"""
int main() {
    D3D12::RecordedDrawState state;
    state.blend.Update({1, 2, 3, 4});
    const auto payload = state.blend.value;
    state.Refresh(1);
    assert(state.blend.value == payload);
    assert(state.blend.Update(payload));
}
"""
        )
        for program in (payload_driver, driver):
            with tempfile.TemporaryDirectory(prefix="nxbox-recorded-reset-") as directory:
                source = Path(directory) / "test.cpp"
                binary = Path(directory) / "test"
                source.write_text(program)
                build = subprocess.run(
                    [
                        "clang++",
                        "-std=c++20",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-fsanitize=address,undefined",
                        "-I",
                        str(ROOT / "src"),
                        str(source),
                        "-o",
                        str(binary),
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(build.returncode, 0, build.stderr)
                subprocess.run([str(binary)], check=True, timeout=15)

    def test_fallback_and_single_constants_parameter_wiring(self):
        context = (NATIVE / "d3d12_context.h").read_text()
        self.assertIn('BisectEnabled("NXBOX_D3D12_FULL_STATE_RESET")', context)
        self.assertIn("recorded_state.Refresh(recording_generation, full_state_reset)", context)
        roots = (NATIVE / "d3d12_root_signature.cpp").read_text()
        self.assertEqual(roots.count("D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS"), 1)
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        self.assertIn("recorded.root_args.constants.Update(words.data(), words.size())", rasterizer)
        self.assertIn("SetGraphicsRoot32BitConstants(0,", rasterizer)


if __name__ == "__main__":
    unittest.main()
