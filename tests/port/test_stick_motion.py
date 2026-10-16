# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual portable Xbox motion and gesture state on the host."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cmath>
#include "eden_uwp/stick_motion.h"
using namespace EdenXbox;
using namespace std::chrono_literals;
bool near(float a, float b) { return std::abs(a - b) < .0001f; }
void level(const StickMotionSample& s) {
    assert(near(s.accel[0], 0) && near(s.accel[1], 0) && near(s.accel[2], -1));
}
int main() {
    const auto t = StickMotion::Clock::time_point{};
    StickMotion motion;
    level(motion.Update(false, 1, 1, t)); // Camera input does not move the sensor.
    auto s = motion.Update(true, 0, 0, t + 10ms);
    level(s);
    assert(s.suppress_right_stick && !s.minus_pressed);
    s = motion.Update(true, 1, 0, t + 110ms);
    assert(near(std::atan2(s.accel[0], -s.accel[2]), StickMotion::kMaxTilt));
    assert(near(s.accel[2], -.5f));
    assert(near(s.gyro[1], (1.f / 6) / .1f)); // 60 degrees in 100 ms, turns/s.
    assert(s.delta_timestamp == 100000);
    s = motion.Update(true, 1, 0, t + 120ms);
    assert(near(s.gyro[0], 0) && near(s.gyro[1], 0) && near(s.gyro[2], 0));
    s = motion.Update(false, 0, 0, t + 130ms);
    assert(!s.minus_pressed && !s.suppress_right_stick);
    s = motion.Update(false, 0, 0, t + 205ms);
    assert(near(std::atan2(s.accel[0], -s.accel[2]), StickMotion::kMaxTilt / 2));
    assert(s.gyro[1] < 0);
    level(motion.Update(false, 0, 0, t + 280ms));
    s = motion.Update(false, 0, 0, t + 300ms);
    level(s);
    for (float g : s.gyro) assert(g == 0);

    StickMotion pitch;
    pitch.Update(true, 0, 0, t);
    s = pitch.Update(true, 0, 1, t + 100ms);
    assert(near(std::atan2(-s.accel[1], -s.accel[2]), StickMotion::kMaxTilt));
    assert(s.gyro[0] > 0 && near(s.gyro[1], 0));
    s = pitch.Update(true, -1, -1, t + 200ms);
    assert(near(s.accel[0]*s.accel[0] + s.accel[1]*s.accel[1] +
                s.accel[2]*s.accel[2], 1));
    for (float g : s.gyro) assert(std::isfinite(g) && std::abs(g) <= 2.001f);

    // Combined pitch/roll rates must agree with the moving gravity vector, including yaw.
    StickMotion combined;
    combined.Update(true, .5f, .5f, t);
    const auto before = combined.Update(true, .5f, .5f, t + 100ms);
    const auto after = combined.Update(true, .501f, .501f, t + 101ms);
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        const int k = (i + 2) % 3;
        const float aj = (before.accel[j] + after.accel[j]) / 2;
        const float ak = (before.accel[k] + after.accel[k]) / 2;
        const float derivative = (after.accel[i] - before.accel[i]) / .001f;
        assert(std::abs(derivative - 2 * StickMotion::kPi *
                        (aj * after.gyro[k] - ak * after.gyro[j])) < .001f);
    }

    StickMotion tap;
    tap.Update(true, .1f, -.1f, t); // Ignore stick noise inside the deadzone.
    assert(tap.Update(false, 0, 0, t + 249ms).minus_pressed);
    assert(!tap.Update(false, 0, 0, t + 260ms).minus_pressed); // One poll pulse.
    tap.Update(true, 0, 0, t + 300ms);
    assert(!tap.Update(false, 0, 0, t + 550ms).minus_pressed);
    tap.Update(true, 0, 0, t + 600ms);
    tap.Update(true, .2f, 0, t + 610ms);
    tap.Update(true, 0, 0, t + 620ms);
    assert(!tap.Update(false, 0, 0, t + 630ms).minus_pressed);
    tap.Update(true, 0, 0, t + 700ms);
    tap.Update(true, 0, 0, t + 710ms, true); // HOME or overlay cancels the tap.
    assert(!tap.Update(false, 0, 0, t + 720ms).minus_pressed);
    tap.Update(true, 0, 0, t + 800ms, true);
    assert(!tap.Update(false, 0, 0, t + 810ms).minus_pressed);
    // A repeated timestamp is finite; a long scheduling stall uses a HID-valid interval.
    s = tap.Update(true, 1, 1, t + 810ms);
    for (float g : s.gyro) assert(std::isfinite(g));
    assert(tap.Update(true, 1, 1, t + 2s).delta_timestamp <= 100000);
}
"""


def test_stick_motion_math_and_gesture():
    with tempfile.TemporaryDirectory(prefix="nxbox-motion-") as temporary:
        source = Path(temporary) / "driver.cpp"
        binary = Path(temporary) / "driver"
        source.write_text(DRIVER)
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(binary)], check=True, timeout=10)
