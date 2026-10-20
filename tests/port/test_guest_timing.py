# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure guest clock rate and host input gaps independently of presented FPS."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class GuestTimingTests(unittest.TestCase):
    def test_clock_rates_poll_gaps_and_pause_reset(self):
        compiler = shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "timing.cpp"
            executable = Path(directory) / "timing"
            source.write_text(
                r"""
#include <cassert>
#include <cmath>
#include "eden_uwp/timing_probe.h"
int main() {
    EdenXbox::TimingProbe probe;
    // Large epochs must cancel before conversion to floating point.
    constexpr std::int64_t epoch = 8'000'000'000'000'000;
    constexpr std::uint64_t ticks = 100'000'000'000'000;
    probe.Reset(epoch, ticks, ticks);
    probe.Poll(epoch + 8'000'000);
    probe.Poll(epoch + 16'000'000);
    probe.Poll(epoch + 116'000'000);
    auto sample = probe.Read(epoch + 5'000'000'000, ticks + 96'000'000,
                             ticks + 96'000'000);
    assert(sample.wall_ms == 5000.0);
    assert(std::abs(sample.clock_mhz - 19.2) < 0.000001);
    assert(std::abs(sample.guest_mhz - 19.2) < 0.000001);
    assert(sample.polls == 3 && sample.max_poll_gap_ms == 100.0);
    // Boost/overclock is distinguishable from a bad underlying wall clock.
    sample = probe.Read(epoch + 5'000'000'000, ticks + 96'000'000,
                        ticks + 192'000'000);
    assert(sample.clock_mhz == 19.2 && sample.guest_mhz == 38.4);
    sample = probe.Read(epoch + 5'000'000'000, ticks + 192'000'000,
                        ticks + 192'000'000);
    assert(sample.clock_mhz == 38.4 && sample.guest_mhz == 38.4);
    // Resume and each reporting interval start fresh, without a false input hitch.
    probe.Reset(epoch + 60'000'000'000, ticks, ticks);
    probe.Poll(epoch + 60'008'000'000);
    sample = probe.Read(epoch + 61'000'000'000, ticks + 19'200'000,
                        ticks + 19'200'000);
    assert(sample.polls == 1 && sample.max_poll_gap_ms == 8.0);
    assert(sample.wall_ms == 1000.0 && sample.guest_mhz == 19.2);
    sample = probe.Read(epoch + 60'000'000'000, ticks, ticks);
    assert(sample.clock_mhz == 0.0 && sample.guest_mhz == 0.0);
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
                    "-Wshadow",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)

    def test_probe_is_wired_after_settings_and_independent_of_frames(self):
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn('"TIMING_POLICY ', source)
        self.assertIn('"GAME_TIMING ', source)
        self.assertIn("timing_probe.Poll(timing_now());", source)
        self.assertIn("system.CoreTiming().GetClockTicks()", source)
        self.assertIn("Common::g_wall_clock.GetCNTPCT()", source)
        self.assertLess(
            source.index("ApplyEdenSettingsFile(TitleSettings::File(local_state, title))"),
            source.index('"TIMING_POLICY '),
        )
        self.assertEqual(source.count("reset_timing_probe();"), 3)


if __name__ == "__main__":
    unittest.main()
