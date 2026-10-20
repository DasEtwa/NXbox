# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host behavioral invariants, native build sharing, and real-cache parity."""

import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <atomic>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_compile_policy.h"
#include "video_core/renderer_d3d12/d3d12_native_pso_cache.h"
#include "video_core/renderer_d3d12/d3d12_build_stats.h"
using namespace D3D12;
using namespace D3D12::Pso;
// An independent effective-descriptor projection: emit only fields consumed by
// D3D12 under this state's enables. All other state/binding bytes are retained.
std::vector<unsigned> Behavior(const GraphicsPsoState& s) {
    std::vector<unsigned> out;
    const auto add = [&](auto v) { out.push_back(static_cast<unsigned>(v)); };
    add(s.topology_type); add(s.cull_mode); add(s.fill_mode); add(s.front_ccw);
    add(s.cull_all); add(s.depth_clip); add(s.depth_enable);
    if (s.depth_enable) { add(s.depth_write); add(s.depth_func); }
    add(s.stencil_enable);
    if (s.stencil_enable) {
        add(s.stencil_read_mask); add(s.stencil_write_mask);
        if (s.stencil_enable & 2) {
            add(s.stencil_back_read_mask); add(s.stencil_back_write_mask);
        }
        for (auto face : {s.front, s.back}) {
            add(face.fail); add(face.depth_fail); add(face.pass); add(face.func);
        }
    }
    add(s.independent_blend); add(s.num_render_targets); add(s.sample_count);
    if (s.topology_type == kTopologyTypePatch) add(s.patch_vertices);
    add(s.logic_enable);
    if (s.logic_enable) add(s.logic_op);
    add(s.alpha_to_coverage); add(s.conservative_raster); add(s.smooth_lines);
    add(s.depth_bounds); add(s.depth_bias); add(s.slope_scaled_bias_bits);
    add(s.bias_clamp_bits); add(s.dsv_format);
    for (unsigned i = 0; i < 8; ++i) {
        add(s.rtv_formats[i]);
        if (i >= s.num_render_targets || !s.rtv_formats[i]) continue;
        const auto& b = s.blend[s.independent_blend ? i : 0];
        add(b.enable); add(b.write_mask);
        if (b.enable) {
            add(b.src); add(b.dst); add(b.op);
            add(b.src_alpha); add(b.dst_alpha); add(b.op_alpha);
        }
    }
    for (const auto& a : s.attributes) {
        add(a.enabled); add(a.slot); add(a.offset); add(a.dxgi);
        add(a.instanced); add(a.swap_rb); add(a.scaled); add(a.pad);
        if (a.instanced) add(a.divisor);
    }
    return out;
}
int main(int argc, char**) {
    static_assert(sizeof(GraphicsPsoState) == 656);
    static_assert(ParseDxcOptimization("") == 3 && ParseDxcOptimization("9") == 3);
    static_assert(ParseDxcOptimization("0") == 0 && ParseDxcOptimization("1") == 1);
    static_assert(ParseDxcOptimization("2") == 2 && ParseDxcOptimization("3") == 3);
    if (argc > 1) {
        GraphicsPsoState s;
        while (std::cin.read(reinterpret_cast<char*>(&s), sizeof(s))) {
            NormalizeGraphicsState(s);
            std::cout.write(reinterpret_cast<const char*>(&s), sizeof(s));
        }
        return 0;
    }
    std::mt19937 random{1937};
    for (unsigned n = 0; n < 50000; ++n) {
        GraphicsPsoState before;
        auto* bytes = reinterpret_cast<unsigned char*>(&before);
        for (unsigned i = 0; i < sizeof(before); ++i) bytes[i] = random();
        before.depth_enable &= 1; before.stencil_enable &= 3;
        before.independent_blend &= 1; before.logic_enable &= 1;
        before.num_render_targets %= 9;
        for (auto& format : before.rtv_formats) format = random() % 3 ? 28 : 0;
        // IndependentBlendEnable=false applies slot zero even across an RTV hole.
        // Preserve slot zero in the normalization in all such cases.
        auto after = before;
        NormalizeGraphicsState(after);
        assert(Behavior(before) == Behavior(after));
        const auto once = after;
        NormalizeGraphicsState(after);
        assert(std::memcmp(&once, &after, sizeof(after)) == 0);
    }
    std::array<std::uint8_t, 40> recipe{};
    auto key = NativeGraphicsRecipeIdentity(recipe);
    recipe[8] = 5;
    assert(key == NativeGraphicsRecipeIdentity(recipe));
    recipe[12] = 1;
    assert(key != NativeGraphicsRecipeIdentity(recipe));
    NativePsoCache<unsigned> cache;
    std::atomic<unsigned> calls{};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 12; ++i)
        threads.emplace_back([&] {
            assert(cache.GetOrBuild(key, [&] { ++calls; return 42u; }) == 42);
        });
    for (auto& thread : threads) thread.join();
    assert(calls == 1);
    for (unsigned i = 0; i < 2; ++i) {
        try {
            cache.GetOrBuild({7, 9}, [&]() -> unsigned { ++calls; throw 17; });
            assert(false);
        } catch (int error) { assert(error == 17); }
    }
    assert(calls == 3);
    assert(cache.GetOrBuild({7, 9}, [&] { ++calls; return 19u; }) == 19);
    assert(calls == 4);
    PsoBuildStats stats;
    {
        ScopedPsoBuildStats collect{stats};
        assert(TimePsoDriver([] { return 12; }) == 12);
        assert(stats.driver_builds == 1);
    }
    assert(!active_build_stats);
    std::cout << "50000 random states: effective descriptors unchanged; native build sharing OK\n";
}
"""


class PsoNormalizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        source = Path(cls.temp.name) / "driver.cpp"
        source.write_text(DRIVER)
        cls.binary = Path(cls.temp.name) / "driver"
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-pthread",
                "-fsanitize=address,undefined",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(cls.binary),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_effective_descriptors_and_native_cache(self):
        subprocess.run([str(self.binary)], check=True)

    def test_real_cache_cpp_python_parity(self):
        folder = os.environ.get("NXBOX_PSO_CENSUS_DIR")
        if not folder:
            self.skipTest("set NXBOX_PSO_CENSUS_DIR to the read-only c24 artifact folder")
        spec = importlib.util.spec_from_file_location("census", ROOT / "tools/nxbox/pso_census.py")
        census = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(census)
        count = 0
        expected = {
            "mk8d-orig": (248, 1035),
            "botw-orig": (802, 611),
            "ody-orig": (618, 893),
            "wonder-orig": (311, 791),
            "all-kirby-orig": (406, 410),
            "all-bf-orig": (167, 757),
            "all-dbsz-orig": (173, 288),
            "all-la-orig": (0, 26),
        }
        for name in (
            "mk8d-orig",
            "botw-orig",
            "ody-orig",
            "wonder-orig",
            "all-kirby-orig",
            "all-bf-orig",
            "all-dbsz-orig",
            "all-la-orig",
        ):
            game = Path(folder) / name
            records = [census.read_record(p) for p in sorted(game.glob("*.graphics"))]
            states = [s for r in records for s in (r["state"], r["request"][48:])]
            result = subprocess.run(
                [str(self.binary), "normalize"],
                input=b"".join(states),
                stdout=subprocess.PIPE,
                check=True,
            ).stdout
            self.assertEqual(result, b"".join(census.normalize(s) for s in states), game.name)
            normalized_requests = set()
            native_keys = set()
            for i, r in enumerate(records):
                recipe_state = result[i * 1312 : i * 1312 + 656]
                request_state = result[i * 1312 + 656 : (i + 1) * 1312]
                normalized_requests.add(r["request"][:48] + request_state)
                eligible = (
                    r["feedback"] == bytes(4) and not recipe_state[21] and not (recipe_state[9] & 2)
                )
                native_keys.add(
                    (
                        r["stage_recipe"],
                        r["root"],
                        recipe_state,
                        r["feedback"],
                        b"" if eligible else r["topology"],
                    )
                )
            reduction = len({r["request"] for r in records}) - len(normalized_requests)
            self.assertEqual((reduction, len(native_keys)), expected[name], name)
            print(f"{name}: C++ request reduction={reduction}, native recipes={len(native_keys)}")
            count += len(records)
        self.assertEqual(count, 7685)
        print(f"{count} real graphics records: C++/Python normalization byte-identical")


if __name__ == "__main__":
    unittest.main()
