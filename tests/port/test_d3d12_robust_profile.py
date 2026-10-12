# SPDX-License-Identifier: GPL-3.0-or-later
"""Fault injection for renderer containment and CPU/wait accounting without a GPU."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include "eden_uwp/renderer_boundary.h"
#include "eden_uwp/thread_profile.h"
int main() {
    int reports = 0;
    const char* reason = nullptr;
    auto report = [&](const std::exception& error) noexcept {
        ++reports;
        reason = error.what();
        return true;
    };
    assert(EdenXbox::RendererBoundary("unused", [] {}, report));
    assert(reports == 0);
    assert(!EdenXbox::RendererBoundary("unused", [] { throw std::runtime_error("unsupported opcode 19"); },
        [&](const std::exception& error) { assert(std::strcmp(error.what(), "unsupported opcode 19") == 0); return report(error); }));
    assert(!EdenXbox::RendererBoundary("GPU command: non-standard exception", [] { throw 19; }, report));
    assert(std::strcmp(reason, "GPU command: non-standard exception") == 0);
    // The next command and its fence still run after either type of failure.
    int fences = 0;
    for (int i = 0; i < 3; ++i) {
        EdenXbox::RendererBoundary("unknown", [i] { if (i == 1) throw 1; }, report);
        ++fences;
    }
    assert(fences == 3 && reports == 3);
    // Backends that decline containment preserve both original exception types.
    auto decline = [](const std::exception&) { return false; };
    try {
        EdenXbox::RendererBoundary("unknown", [] { throw 42; }, decline);
        assert(false);
    } catch (int value) { assert(value == 42); }
    try {
        EdenXbox::RendererBoundary("unused", [] { throw std::runtime_error("OpenGL"); }, decline);
        assert(false);
    } catch (const std::runtime_error& error) { assert(std::strcmp(error.what(), "OpenGL") == 0); }
    using namespace EdenXbox::ThreadProfile;
    CpuSample sample;
    assert(sample.Update(10, 100, 1000) == 0);
    assert(sample.Update(10, 50000100, 6000) == 100);
    assert(sample.Update(10, 62500100, 11000) == 25);
    // Delayed timer: 5 s CPU / 10 s measured wall = 50%, not 100%.
    assert(sample.Update(10, 112500100, 21000) == 50);
    assert(sample.Update(10, 112500100, 26000) == 0);
    // Reused IDs, backward CPU counters and zero-duration windows reset the baseline.
    assert(sample.Update(11, 0, 31000) == 0);
    assert(sample.Update(11, 100, 31000) == 0);
    assert(sample.Update(11, 0, 36000) == 0);
    assert(Enabled());
    {
        Scope present{Wait::Present};
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    try { Scope fence{Wait::Fence}; throw 1; } catch (int) {}
    assert(waits[0].calls.load() == 1 && waits[0].microseconds.load() >= 1000);
    assert(waits[1].calls.load() == 1);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([] { for (int j = 0; j < 100; ++j) { Scope scope{Wait::Fence}; } });
    for (auto& worker : workers) worker.join();
    assert(waits[1].calls.exchange(0) == 801);
    assert(waits[1].calls.load() == 0);
}
"""


class RobustProfileTests(unittest.TestCase):
    def test_fault_injection_and_cpu_accounting(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-t4-") as temp:
            source = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
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
            subprocess.run(
                [str(binary)],
                env={**os.environ, "NXBOX_THREAD_PROFILE": "1"},
                check=True,
                timeout=15,
            )

    def test_containment_sites(self):
        sites = {
            "gpu_thread.cpp": [
                "GPU initialization: non-standard exception",
                "GPU context release: non-standard exception",
                "GPU command: non-standard exception",
                "GPU cache invalidation: non-standard exception",
            ],
            "renderer_d3d12/renderer_d3d12.cpp": [
                "D3D12 renderer teardown: non-standard exception",
                "D3D12 compositor callback: non-standard exception",
                "D3D12 frame notification: non-standard exception",
                "D3D12 presentation initialization: non-standard exception",
                "D3D12 applet capture: non-standard exception",
            ],
            "renderer_d3d12/d3d12_context.cpp": [
                "D3D12 context teardown: non-standard exception",
                "D3D12 completion callback: non-standard exception",
            ],
        }
        for file, reasons in sites.items():
            source = (ROOT / "src/video_core" / file).read_text()
            for reason in reasons:
                with self.subTest(site=reason):
                    self.assertIn(reason, source)
        self.assertEqual(sum(map(len, sites.values())), 11)

    def test_profile_covers_process_and_reports_missing_access(self):
        source = (ROOT / "src/eden_uwp/crash_logger.cpp").read_text()
        for required in [
            "GetThreadTimes",
            "GetThreadDescription",
            "Thread32First",
            "Thread32Next",
            "entry.process == GetCurrentProcessId()",
            "complete={}",
            "inaccessible={}",
            "std::chrono::seconds(5)",
            "host_wait={}",
            "THREAD_QUERY_LIMITED_INFORMATION",
        ]:
            with self.subTest(required=required):
                self.assertIn(required, source)
