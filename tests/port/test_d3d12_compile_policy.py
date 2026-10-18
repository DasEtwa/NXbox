# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile policy decisions and synchronized worker priority transitions without a Windows SDK."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <future>
#include <mutex>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_async_pso.h"
using namespace std::chrono_literals;
using Pool = D3D12::AsyncPsoWorkerPool<int, int>;
using namespace D3D12;
int main() {
    for (bool enabled : {false, true})
        for (bool compute : {false, true})
            for (bool global : {false, true})
                assert(SkipGlobalFallbackCompute(enabled, compute, global) ==
                       (enabled && compute && global));
    unsetenv("NXBOX_D3D12_SKIP_GLOBAL_FALLBACK_COMPUTE");
    assert(SkipGlobalFallbackComputeEnabled() == XboxCompilePolicy);
    setenv("NXBOX_D3D12_SKIP_GLOBAL_FALLBACK_COMPUTE", "0", 1);
    assert(!SkipGlobalFallbackComputeEnabled());
    setenv("NXBOX_D3D12_SKIP_GLOBAL_FALLBACK_COMPUTE", "1", 1);
    assert(SkipGlobalFallbackComputeEnabled());
    unsetenv("NXBOX_D3D12_PSO_WORKER_PRIORITY");
    assert(PsoWorkerPriority() == -1);
    assert(ParsePsoWorkerPriority("lowest") == -2);
    assert(ParsePsoWorkerPriority("below") == -1);
    assert(ParsePsoWorkerPriority("normal") == 0);
    for (auto bad : {"", "LOWEST", "1", "normal "})
        assert(ParsePsoWorkerPriority(bad) == -1);
    unsetenv("NXBOX_D3D12_COMPILE_HEADROOM_MB");
    assert(CompileHeadroomBytes() == (XboxCompilePolicy ? 448ULL * 1024 * 1024 : 0));
    setenv("NXBOX_D3D12_COMPILE_HEADROOM_MB", "64", 1);
    assert(CompileHeadroomBytes() == 64ULL * 1024 * 1024);
    setenv("NXBOX_D3D12_COMPILE_HEADROOM_MB", "bad", 1);
    assert(CompileHeadroomBytes() == (XboxCompilePolicy ? 448ULL * 1024 * 1024 : 0));
    CompileHeadroomGuard guard;
    auto now = CompileHeadroomGuard::Clock::time_point{};
    unsigned reads{};
    std::uint64_t usage = 500;
    auto read = [&] { ++reads; return std::pair{1000ULL, usage}; };
    assert(guard.Allows(448, now, read) && reads == 1);
    usage = 600;
    assert(guard.Allows(448, now + 99ms, read) && reads == 1);
    assert(!guard.Allows(448, now + 100ms, read) && reads == 2);
    assert(guard.Headroom() == 400);
    assert(guard.ShouldLog(now));
    assert(!guard.ShouldLog(now + 4999ms));
    assert(guard.ShouldLog(now + 5s));
    usage = 552;
    assert(guard.Allows(448, now + 200ms, read));
    usage = 1100;
    assert(!guard.Allows(448, now + 300ms, read) && guard.Headroom() == 0);
    assert(guard.Allows(0, now + 500ms, read) && reads == 4);

    {
        Pool deferred{1, 1};
        auto job = [] { return std::make_shared<int>(1); };
        if (guard.Allows(448, now + 600ms, read))
            deferred.Submit(1, job);
        assert(deferred.Lookup(1).status == Pool::Status::Missing);
        usage = 100;
        if (guard.Allows(448, now + 700ms, read))
            deferred.Submit(1, job);
        assert(deferred.WaitFor(1, 2s).status == Pool::Status::Ready);
    }
    // Apply the knob at job start, boost a running job, keep it boosted until
    // the last of two timed-out waiters leaves, and restore again on completion.
    for (auto value : {"lowest", "below", "normal", "invalid"}) {
        setenv("NXBOX_D3D12_PSO_WORKER_PRIORITY", value, 1);
        std::mutex mutex;
        std::vector<int> priorities;
        auto hook = [&](Pool::NativeHandle, int priority) {
            std::scoped_lock lock{mutex};
            priorities.push_back(priority);
        };
        Pool pool{1, 4, 5s, 1, hook};
        std::promise<void> started, release;
        auto gate = release.get_future().share();
        assert(pool.Submit(1, [&] {
            started.set_value(); gate.wait(); return std::make_shared<int>(1);
        }));
        started.get_future().wait();
        {
            std::scoped_lock lock{mutex};
            assert(priorities == std::vector<int>{ParsePsoWorkerPriority(value)});
        }
        auto second = std::async(std::launch::async, [&] { return pool.WaitFor(1, 5s); });
        while (pool.WaiterCount(1) != 1) std::this_thread::yield();
        assert(pool.WaitFor(1, 40ms).status == Pool::Status::Pending);
        assert(pool.WaiterCount(1) == 1);
        {
            std::scoped_lock lock{mutex};
            assert(priorities.back() == 1);
        }
        release.set_value();
        assert(second.get().status == Pool::Status::Ready);
        {
            std::scoped_lock lock{mutex};
            assert(priorities.back() == ParsePsoWorkerPriority(value));
        }
        // Last waiter leaving while compilation is still running restores base.
        std::promise<void> started2, release2;
        auto gate2 = release2.get_future().share();
        assert(pool.Submit(2, [&] {
            started2.set_value(); gate2.wait(); return std::make_shared<int>(2);
        }));
        started2.get_future().wait();
        assert(pool.WaitFor(2, 1ms).status == Pool::Status::Pending);
        {
            std::scoped_lock lock{mutex};
            assert(priorities[priorities.size() - 2] == 1);
            assert(priorities.back() == ParsePsoWorkerPriority(value));
        }
        release2.set_value();
        assert(pool.WaitFor(2, 2s).status == Pool::Status::Ready);
    }
    // Queue promotion, including boost when a waited key starts running.
    std::mutex mutex;
    std::vector<int> order;
    int priority{};
    Pool pool{1, 4, 5s, 1, [&](Pool::NativeHandle, int value) {
        std::scoped_lock lock{mutex}; priority = value;
    }};
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    assert(pool.Submit(1, [&] {
        started.set_value(); gate.wait(); return std::make_shared<int>(1);
    }));
    started.get_future().wait();
    assert(pool.Submit(2, [&] {
        std::scoped_lock lock{mutex}; order.push_back(2); return std::make_shared<int>(2);
    }));
    assert(pool.Submit(3, [&] {
        std::scoped_lock lock{mutex};
        assert(priority == 1); order.push_back(3); return std::make_shared<int>(3);
    }));
    auto waited = std::async(std::launch::async, [&] { return pool.WaitFor(3, 2s); });
    while (pool.WaiterCount(3) != 1) std::this_thread::yield();
    release.set_value();
    assert(waited.get().status == Pool::Status::Ready);
    assert(pool.WaitFor(2, 2s).status == Pool::Status::Ready);
    assert((order == std::vector<int>{3, 2}));
    assert(pool.Reject(4, "Global memory fallback"));
    assert(pool.Lookup(4).status == Pool::Status::Failed);
    assert(!pool.Submit(4, [] { assert(false); return std::make_shared<int>(4); }));
}
"""


class CompilePolicyTests(unittest.TestCase):
    def test_policy_and_priorities(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            source = Path(directory) / "policy.cpp"
            source.write_text(DRIVER)
            for defines in ([], ["-DNXBOX_UWP"]):
                binary = Path(directory) / "policy"
                result = subprocess.run(
                    [
                        os.environ.get("CXX", "c++"),
                        "-std=c++20",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-pthread",
                        *defines,
                        "-I",
                        str(ROOT / "src"),
                        str(source),
                        "-o",
                        str(binary),
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(binary)], check=True, timeout=20)

    def test_policy_wiring(self):
        path = ROOT / "src/video_core/renderer_d3d12"
        cache = (path / "d3d12_pipeline_cache.cpp").read_text()
        self.assertNotIn("SetThreadPriority(", cache)
        # Draws and dispatches admit a compile when the pool is idle, or low memory held
        # elsewhere would defer their keys forever; warmup keys can wait.
        self.assertEqual(cache.count("!CompileHeadroomAvailable(async->Idle())"), 2)
        self.assertEqual(cache.count("!CompileHeadroomAvailable(false)"), 1)
        self.assertIn("if (allowed || pool_idle)\n        return true;", cache)
        compute = cache.split("ComputePipeline* PipelineCache::CurrentComputePipeline()", 1)[1]
        self.assertLess(compute.index("async->Reject("), compute.index("async->Submit("))
        # A translation that throws on the GPU thread must leave a Failed entry, not a Missing one.
        self.assertLess(
            compute.index('async->Reject(async_key, "Compute translation failed");'),
            compute.index("async->Submit("),
        )
        for kind in ("compute", "graphics"):
            self.assertIn(f"kind={kind} wall_ms=", cache)
        self.assertIn("PSO_TRACE compute={:016x} {} wall_ms=", cache)
