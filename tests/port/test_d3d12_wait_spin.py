# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the actual bounded fence poller without Windows or a graphics SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "video_core/renderer_d3d12/d3d12_completion.h"
using namespace std::chrono_literals;
int main() {
    int time = 0, polls = 0, pauses = 0;
    auto clock = [&] { return time++; };
    auto pause = [&] { ++pauses; };
    assert(!D3D12::SpinUntilReady([&] { ++polls; return false; }, clock, pause, 5));
    assert(polls == 4 && pauses == 4 && time == 6);
    time = polls = pauses = 0;
    assert(!D3D12::SpinUntilReady([&] { ++polls; return true; }, clock, pause, 0));
    assert(polls == 0 && pauses == 0);
    time = polls = pauses = 0;
    assert(D3D12::SpinUntilReady([&] { return ++polls == 2; }, clock, pause, 5));
    assert(polls == 2 && pauses == 1);
    try {
        time = 0;
        D3D12::SpinUntilReady([]() -> bool { throw std::runtime_error("device lost"); },
                             clock, pause, 5);
        assert(false);
    } catch (const std::runtime_error&) {}
    // Completion before polling, during polling, or after fallback registration.
    // The fallback predicate must recheck completion under its own wait protocol.
    for (int i = 0; i < 300; ++i) {
        std::atomic<bool> ready{i % 3 == 0};
        std::mutex mutex;
        std::condition_variable cv;
        std::jthread completion([&] {
            if (i % 3 == 2)
                std::this_thread::sleep_for(200us);
            std::lock_guard lock{mutex};
            ready.store(true, std::memory_order_release);
            cv.notify_one();
        });
        const bool spun = D3D12::SpinUntilReady(
            [&] { return ready.load(std::memory_order_acquire); },
            [] { return std::chrono::steady_clock::now(); }, [] {}, 50us);
        if (!spun) {
            std::unique_lock lock{mutex};
            assert(cv.wait_for(lock, 2s, [&] { return ready.load(std::memory_order_acquire); }));
        }
        assert(ready);
    }
}
"""


class D3D12WaitSpinTests(unittest.TestCase):
    def test_bounded_polling_and_completion_races(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-wait-spin-") as temp:
            source = Path(temp) / "wait.cpp"
            binary = Path(temp) / "wait"
            source.write_text(DRIVER)
            for flags in ([], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]):
                with self.subTest(flags=flags):
                    subprocess.run(
                        [
                            "clang++",
                            "-std=c++20",
                            "-pthread",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            *flags,
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
                    subprocess.run([str(binary)], check=True, timeout=15)

    def test_wait_submitted_spins_then_blocks_on_its_own_event(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        start = source.index("void Context::WaitSubmitted(")
        body = source[start : source.index("\n}\n", start)]
        self.assertIn("NXBOX_D3D12_WAIT_SPIN", source)
        self.assertIn('Diagnostic(fmt::format("D3D12 wait spin={}"', source)
        fallback = body.index("if (!spun_ready)")
        self.assertLess(body.index("SpinUntilReady("), fallback)
        for call in (
            "CreateEventExW",
            "SetEventOnCompletion",
            "WaitForSingleObjectEx(wait_event, 10000",
        ):
            self.assertGreater(body.index(call), fallback, call)
        self.assertGreater(body.index("CompleteThrough(tick)"), fallback)

    def test_wait_origins_are_recorded(self):
        for path, marker in (
            ("src/video_core/dma_pusher.cpp", "Wait::DmaMemorySync"),
            ("src/video_core/engines/puller.cpp", "Wait::PullerAcquire"),
            ("src/video_core/renderer_d3d12/d3d12_fence_manager.cpp", "Wait::GuestFenceCompletion"),
            ("src/video_core/renderer_d3d12/d3d12_rasterizer.cpp", "Wait::ForcedDrain"),
        ):
            self.assertIn(marker, (ROOT / path).read_text(), path)
        self.assertIn('"forced_drain"', (ROOT / "src/common/gpu_thread_profile.h").read_text())
