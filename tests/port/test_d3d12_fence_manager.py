# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the shared fence protocol with std-only fake caches and a GPU queue."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STUBS = r"""
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <vector>
using u32 = std::uint32_t;
namespace Settings {
inline bool IsGPUFenceBehaviorDefault() { return true; }
inline bool IsGPULevelHigh() { return true; }
inline bool IsGPUFenceBehaviorBalanced() { return false; }
inline bool IsGPUFenceBehaviorAccurate() { return true; }
}
namespace Common {
enum class ThreadPriority { High };
inline void SetCurrentThreadName(const char*) {}
inline void SetCurrentThreadPriority(ThreadPriority) {}
inline void SetCurrentThreadToPerformanceCores() {}
}
namespace VideoCommon {
template <typename T, int N> struct DelayedDestructionRing {
    void Tick() {}
    void Push(T) {}
};
}
namespace Tegra {
namespace Host1x {
struct SyncpointManager {
    int guest{}, host{};
    void IncrementGuest(u32) { ++guest; }
    void IncrementHost(u32) { ++host; }
};
}
struct Host { Host1x::SyncpointManager sync; auto& GetSyncpointManager() { return sync; } };
struct GPU { Host host; auto& Host1x() { return host; } };
}
namespace VideoCore {
struct RasterizerInterface {
    int submits{}, invalidations{};
    void FlushCommands() { ++submits; }
    void InvalidateGPUCache() { ++invalidations; }
};
}
"""
DRIVER = r"""
#include <cassert>
#include <memory>
#include "video_core/fence_manager.h"
struct Cache {
    std::mutex mutex;
    bool dirty{}, ready{};
    int commits{}, pops{}, pending{};
    std::function<void()> on_commit;
    void AccumulateFlushes() {}
    bool HasUncommittedFlushes() { return dirty; }
    bool ShouldWaitAsyncFlushes() { return pending && !ready; }
    void CommitAsyncFlushes() {
        ++commits; ++pending;
        if (on_commit) on_commit();
        dirty = false;
    }
    void PopAsyncFlushes() { assert(pending > 0); --pending; ++pops; }
};
struct Fence : VideoCommon::FenceBase {
    explicit Fence(bool stubbed) : FenceBase(stubbed) {}
    bool ready{};
};
struct Params {
    using FenceType = std::shared_ptr<Fence>;
    using BufferCacheType = Cache;
    using TextureCacheType = Cache;
    using QueryCacheType = Cache;
    static constexpr bool HAS_ASYNC_CHECK = false;
};
struct Manager : VideoCommon::FenceManager<Params> {
    using Base = VideoCommon::FenceManager<Params>;
    Manager(VideoCore::RasterizerInterface& r, Tegra::GPU& g, Cache& t, Cache& b, Cache& q)
        : Base(r, g, t, b, q) {}
    std::shared_ptr<Fence> latest;
    int waits{};
    std::shared_ptr<Fence> CreateFence(bool stubbed) override {
        latest = std::make_shared<Fence>(stubbed); return latest;
    }
    void QueueFence(std::shared_ptr<Fence>&) override {}
    bool IsFenceSignaled(std::shared_ptr<Fence>& f) const override {
        return f->IsStubbed() || f->ready;
    }
    void WaitFence(std::shared_ptr<Fence>& f) override { ++waits; f->ready = true; }
};
int main() {
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache texture, buffer, query;
    Manager manager(rasterizer, gpu, texture, buffer, query);
    std::vector<int> order;
    bool committing = false;
    // QueryCache::CommitAsyncFlushes schedules unregister while constructing its batch.
    query.on_commit = [&] {
        committing = true;
        manager.SyncOperation([&] {
            assert(!committing);
            assert(query.pops == query.commits);
            order.push_back(2);
        });
        committing = false;
    };
    manager.SyncOperation([&] { assert(query.pops == 1); order.push_back(1); });
    assert(order.empty() && rasterizer.submits == 0);
    query.dirty = true;
    manager.SignalFence([&] { order.push_back(3); });
    assert(order.empty() && query.commits == 1 && query.pops == 0);
    assert(!manager.latest->IsStubbed() && rasterizer.submits == 1);
    manager.WaitPendingFences(true);
    assert((order == std::vector<int>{1, 2, 3}));
    assert(manager.waits == 1 && query.pending == 0);
    // A syncpoint publishes guest progress now, host progress after the fence.
    query.dirty = true;
    manager.SignalSyncPoint(7);
    assert(gpu.host.sync.guest == 1 && gpu.host.sync.host == 0);
    manager.WaitPendingFences(true);
    assert(gpu.host.sync.host == 1 && manager.waits == 2);
    // Empty cache batches still retire and run unregister exactly once.
    manager.SignalReference();
    assert(manager.latest->IsStubbed());
    manager.WaitPendingFences(true);
    assert(query.commits == 3 && query.pops == 3);
    assert(buffer.commits == buffer.pops && texture.commits == texture.pops);
}
"""


class D3D12FenceManagerTests(unittest.TestCase):
    def test_cache_commit_callback_and_syncpoint_ordering(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-fences-") as temp:
            temp = Path(temp)
            (temp / "stubs.h").write_text(STUBS)
            for name in (
                "common/common_types.h",
                "common/settings.h",
                "common/thread.h",
                "video_core/delayed_destruction_ring.h",
                "video_core/gpu.h",
                "video_core/host1x/host1x.h",
                "video_core/host1x/syncpoint_manager.h",
                "video_core/rasterizer_interface.h",
            ):
                header = temp / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "stubs.h"\n')
            source = temp / "fences.cpp"
            binary = temp / "fences"
            source.write_text(DRIVER)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-pthread",
                    "-I",
                    str(temp),
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
            subprocess.run([str(binary)], check=True, timeout=10)
