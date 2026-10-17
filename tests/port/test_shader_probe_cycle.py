# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the standard-library-only cycle clock and nested bucket math on the host."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ShaderProbeCycleTests(unittest.TestCase):
    def test_cycle_boundaries_and_nested_partition(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "cycle.cpp"
            executable = Path(directory) / "cycle"
            source.write_text(
                r"""
#include <cassert>
#include <limits>
#include "common/nxbox_shader_probe.h"
using namespace NxboxShaderProbe;
int main() {
    for (const auto bad : {"", "0:1", "1:0", "-1:2", "2:-1", "2", "2:1:3",
                           "2:1junk", "2:0.5", "18446744073709551615:1",
                           "2:9223372036854776"}) {
        Cycle cycle;
        assert(!cycle.Parse(bad) && cycle.count == 0);
    }
    Cycle cycle;
    assert(cycle.Parse(" 3 : 2 ") && cycle.count == 3 && cycle.duration_ms == 2000);
    for (const auto bad : {"", "0:0", "2:2", "-2:0", "2:-1", "2:0:1", "2"}) {
        assert(!cycle.ParseWithin(bad) && cycle.outer_count == 0);
    }
    CycleState off;
    assert(!off.Advance(Cycle{}, 0) && !off.started);
    assert(!off.TakeProbe(Cycle{}, 10000));
    CycleState state;
    assert(!state.Advance(cycle, 10000)); // first guest draw, not process launch
    assert(!state.Advance(cycle, 13999) && state.phase == 0 && !state.active);
    assert(!state.TakeProbe(cycle, 13999));
    for (Hash ordinal = 0; ordinal < 12; ++ordinal) {
        const Hash tick = 14000 + ordinal * 2000;
        assert(state.Advance(cycle, tick));
        assert(state.ordinal == ordinal && state.phase == ordinal % 4);
        assert(state.start_tick == tick);
        assert(!state.Advance(cycle, tick + 999));
        assert(!state.TakeProbe(cycle, tick + 999));
        assert(state.TakeProbe(cycle, tick + 1000));
        assert(!state.TakeProbe(cycle, tick + 1001));
        assert(!state.Advance(cycle, tick + 1999));
    }
    // Sparse frames jump to the current phase and retain its scheduled start tick.
    assert(state.Advance(cycle, 14000 + 20 * 2000 + 1500));
    assert(state.phase == 0 && state.ordinal == 20 && state.start_tick == 54000);
    assert(state.TakeProbe(cycle, 55500));
    assert(state.Advance(cycle, 56000));
    assert(!state.TakeProbe(cycle, 58000)); // stale frame cannot be mislabeled
    CycleState late;
    assert(!late.Advance(cycle, 10000));
    assert(late.Advance(cycle, 16123) && late.phase == 1 && late.start_tick == 16000);
    CycleState zero;
    assert(!zero.Advance(cycle, 0));
    assert(zero.Advance(cycle, 4000) && zero.phase == 0);
    assert(!zero.Advance(cycle, 3999)); // defensive clock rollback

    for (Hash hash = 0; hash < 10000; ++hash) {
        unsigned matches = 0;
        assert(!cycle.Contains(hash, 0) && !cycle.Contains(hash, 4));
        for (Hash phase = 1; phase <= 3; ++phase) {
            matches += cycle.Contains(hash, phase);
            assert(cycle.Contains(hash, phase) == (hash % 3 == phase - 1));
        }
        assert(matches == 1);
    }
    // Every outer bucket splits into two nonempty, disjoint children, even for 2 -> 2.
    for (Hash n : {2, 3, 4}) {
        for (Hash k = 0; k < n; ++k) {
            Cycle nested;
            assert(nested.Parse("2:1"));
            assert(nested.ParseWithin(std::to_string(n) + ":" + std::to_string(k)));
            unsigned child_counts[2]{};
            for (Hash hash = 0; hash < 10000; ++hash) {
                const bool left = nested.Contains(hash, 1);
                const bool right = nested.Contains(hash, 2);
                assert(!(left && right));
                assert((left || right) == (hash % n == k));
                assert(left == (hash % (n * 2) == k));
                assert(right == (hash % (n * 2) == k + n));
                child_counts[0] += left;
                child_counts[1] += right;
            }
            assert(child_counts[0] != 0 && child_counts[1] != 0);
            const Hash max = std::numeric_limits<Hash>::max();
            assert(nested.Contains(max, 1) == (max % n == k && (max / n) % 2 == 0));
        }
    }
    Config config;
    assert(!config.DrawEnabled());
    assert(config.cycle.ParseWithin("2:0"));
    assert(!config.DrawEnabled()); // WITHIN alone has no draw or clock cost
    assert(config.skip.Parse("0-ffffffffffffffff"));
    assert(config.bucket.Parse("0:0/1"));
    assert(config.cycle.Parse("2:1"));
    assert(config.DrawEnabled() && config.SkipEnabled());
    for (Hash hash = 0; hash < 100; ++hash) {
        assert(!config.Skip(hash, 0)); // baseline overrides static exclusions
        assert(config.Skip(hash, 1) == (hash % 4 == 0));
        assert(config.Skip(hash, 2) == (hash % 4 == 2));
    }
}
"""
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)
