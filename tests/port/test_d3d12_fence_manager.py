# SPDX-FileCopyrightText: Copyright 2026 NSPX
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
inline bool high = true;
inline bool IsGPULevelHigh() { return high; }
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
    std::atomic<int> guest{}, host{};
    u32 GetGuestSyncpointValue(u32) const { return guest.load(); }
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
    std::atomic<int> commits{}, pops{}, pending{};
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
template <bool native> struct Params {
    using FenceType = std::shared_ptr<Fence>;
    using BufferCacheType = Cache;
    using TextureCacheType = Cache;
    using QueryCacheType = Cache;
    static constexpr bool HAS_ASYNC_CHECK = false;
    static constexpr bool REQUIRE_GPU_COMPLETION = native;
};
template <bool native> struct Manager : VideoCommon::FenceManager<Params<native>> {
    using Base = VideoCommon::FenceManager<Params<native>>;
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
    Manager<false> manager(rasterizer, gpu, texture, buffer, query);
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
    // Native GPU work requires a real fence even with no cache readbacks and relaxed settings.
    Settings::high = false;
    query.on_commit = {};
    query.ready = buffer.ready = texture.ready = true;
    Manager<true> native(rasterizer, gpu, texture, buffer, query);
    native.SignalSyncPoint(8);
    assert(!native.latest->IsStubbed());
    assert(gpu.host.sync.guest == 2 && gpu.host.sync.host == 1);
    native.SignalOrdering();
    assert(gpu.host.sync.host == 1 && native.waits == 0);
    native.WaitPendingFences(true);
    assert(gpu.host.sync.host == 2 && native.waits == 1);
    native.SignalReference();
    assert(!native.latest->IsStubbed());
    native.WaitPendingFences(true);
    assert(native.waits == 2);
    assert(buffer.commits == buffer.pops && query.commits == query.pops);
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

    def test_native_syncpoint_retires_without_another_guest_command(self):
        cache = DRIVER.split("struct Cache {", 1)[1].split("struct Fence :", 1)[0]
        native_stubs = (
            STUBS
            + "\nstruct Cache {"
            + cache
            + r"""
using u64 = std::uint64_t;
namespace D3D12 {
enum class FrameCount { ForcedSubmitGuestFence };
using TextureCache = Cache;
using BufferCache = Cache;
using QueryCache = Cache;
struct Context {
    std::atomic<u64> submitted{}, completed{};
    std::atomic<int> waits{};
    bool fail = false;
    std::mutex guard;
    std::condition_variable cv;
    u64 Submit(FrameCount) { return ++submitted; }
    bool IsFree(u64 tick) const { return completed >= tick; }
    void WaitSubmitted(u64 tick) {
        ++waits;
        if (fail) throw std::runtime_error("simulated device failure");
        std::unique_lock lock(guard);
        cv.wait(lock, [&] { return completed >= tick; });
    }
    void ReportFailure(const std::exception&) {}
    void Complete() {
        std::scoped_lock lock(guard);
        completed.store(submitted.load());
        cv.notify_all();
    }
};
inline void Diagnostic(const std::string&) {}
}
"""
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-native-fences-") as temp:
            temp = Path(temp)
            (temp / "stubs.h").write_text(
                "#include <cassert>\n#include <string>\n#include <condition_variable>\n"
                + native_stubs
            )
            for name in (
                "common/common_types.h",
                "common/settings.h",
                "common/thread.h",
                "video_core/delayed_destruction_ring.h",
                "video_core/gpu.h",
                "video_core/host1x/host1x.h",
                "video_core/host1x/syncpoint_manager.h",
                "video_core/rasterizer_interface.h",
                "video_core/renderer_d3d12/d3d12_buffer_cache.h",
                "video_core/renderer_d3d12/d3d12_texture_cache.h",
                "video_core/renderer_d3d12/d3d12_query_cache.h",
                "video_core/buffer_cache/buffer_cache.h",
                "video_core/texture_cache/texture_cache.h",
                "video_core/query_cache/query_cache.h",
            ):
                header = temp / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "stubs.h"\n')
            source = temp / "native.cpp"
            source.write_text(r"""
#include <cassert>
#include "common/gpu_thread_profile.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"
int main() {
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache texture, buffer, query;
    texture.ready = buffer.ready = query.ready = true;
    Settings::high = false;
    D3D12::Context context;
    D3D12::FenceManager manager(rasterizer, gpu, texture, buffer, query, context);
    Common::GpuProfile::Reset();
    Common::GpuProfile::enabled = true;
    Common::GpuProfile::RequestFence(7, 1);
    // Queue multiple submissions without waiting for GPU completion on the producer.
    manager.SignalSyncPoint(7);
    manager.SignalReference();
    std::atomic<bool> callback{};
    manager.SignalFence([&] {
        assert(context.completed >= 3);
        assert(gpu.host.sync.host == 1);
        callback = true;
    });
    assert(context.submitted == 3 && context.completed == 0);
    assert(gpu.host.sync.guest == 1 && gpu.host.sync.host == 0 && !callback);
    // No further guest command or presentation is needed to retire the last fence.
    context.Complete();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callback && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    assert(callback && gpu.host.sync.host == 1);
    const auto profile = Common::GpuProfile::TakeWindow();
    assert(profile.fence_total.count == 1 && profile.fence_queued.count == 1);
    assert(profile.fence_execution.count == 1);
    Common::GpuProfile::enabled = false;
    assert(query.commits == query.pops && buffer.commits == buffer.pops);
    assert(texture.commits == texture.pops && context.waits == 3);
    D3D12::Context broken;
    broken.fail = true;
    D3D12::FenceManager failing(rasterizer, gpu, texture, buffer, query, broken);
    failing.SignalReference();
    bool rejected = false;
    try { failing.WaitPendingFences(true); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected); // A failed completion thread cannot strand a forced drain.

}
""")
            binary = temp / "native"
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
                    str(ROOT / "src/video_core/renderer_d3d12/d3d12_fence_manager.cpp"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
