# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native query pending-list ownership without a graphics SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using VAddr = u64;
enum class QueryType { Counter };
namespace VideoCommon {
enum class QueryFlagBits : u32 { IsInvalidated=1, IsRewritten=2, HasTimestamp=4, IsHostSynced=8 };
QueryFlagBits operator|(QueryFlagBits a, QueryFlagBits b) { return QueryFlagBits(u32(a)|u32(b)); }
QueryFlagBits operator&(QueryFlagBits a, QueryFlagBits b) { return QueryFlagBits(u32(a)&u32(b)); }
QueryFlagBits& operator|=(QueryFlagBits& a, QueryFlagBits b) { return a=a|b; }
bool True(QueryFlagBits a) { return u32(a)!=0; }
struct GuestQuery {
    u64 guest_address, value;
    QueryFlagBits flags;
    GuestQuery(bool timestamp, u64 address, u64 value) : guest_address(address), value(value),
        flags(timestamp ? QueryFlagBits::HasTimestamp : QueryFlagBits(0)) {}
};
struct SyncValuesStruct { u64 address, value, size; };
template<class Query> class SimpleStreamer {
public:
    explicit SimpleStreamer(size_t) {}
    virtual ~SimpleStreamer() = default;
    virtual void StartCounter() = 0;
    virtual void PauseCounter() = 0;
    virtual void CloseCounter() = 0;
    virtual void ResetCounter() = 0;
    virtual size_t WriteCounter(VAddr,bool,u32,std::optional<u32>) = 0;
    virtual bool HasPendingSync() const = 0;
    virtual void SyncWrites() = 0;
    virtual void Free(size_t) { ++freed; }
    std::atomic<int> freed{};
protected:
    template<class... Args> size_t BuildQuery(Args... args) {
        std::scoped_lock lock{guard};
        const auto id=slot_queries.size();
        slot_queries.emplace_back(args...);
        return id;
    }
    std::mutex guard;
    std::deque<Query> slot_queries;
};
}
struct QueryCacheRuntime {
    u64 next{};
    int synced{};
    std::function<void()> on_sync;
    void EnableCounter(QueryType,bool) {}
    void ResetCounter(QueryType) {}
    u64 ReadCounter(QueryType,u32) { return ++next; }
    template<class T> void SyncValues(std::span<T> values) {
        synced += values.size();
        if(on_sync) on_sync();
    }
};
__STREAMER__
int main() {
    QueryCacheRuntime runtime;
    CounterStreamer streamer{QueryType::Counter, runtime};
    // A runtime upload may retire old queries reentrantly. The list lock must
    // already be released when SyncWrites enters that upload.
    const auto first = streamer.WriteCounter(0,true,0);
    runtime.on_sync = [&] { streamer.Free(first); };
    streamer.SyncWrites();
    runtime.on_sync = {};
    assert(streamer.freed == 1 && !streamer.HasPendingSync());
    std::mutex guard;
    std::condition_variable cv;
    std::deque<size_t> retire;
    bool finished = false;
    std::thread worker([&] {
        for (;;) {
            size_t id;
            {
                std::unique_lock lock{guard};
                cv.wait(lock,[&] { return finished || !retire.empty(); });
                if(retire.empty() && finished) break;
                id=retire.front(); retire.pop_front();
            }
            streamer.HasPendingSync();
            streamer.Free(id);
        }
    });
    for(int batch=0; batch<100; ++batch) {
        std::vector<size_t> ids;
        for(int i=0;i<64;++i) ids.push_back(streamer.WriteCounter(8*(batch*64+i),false,0));
        streamer.SyncWrites();
        std::scoped_lock lock{guard};
        retire.insert(retire.end(),ids.begin(),ids.end());
        cv.notify_all();
    }
    { std::scoped_lock lock{guard}; finished=true; cv.notify_all(); }
    worker.join();
    assert(streamer.freed == 6401 && runtime.synced == 6401 && !streamer.HasPendingSync());
}
"""


class D3D12QueryRetirementTests(unittest.TestCase):
    def test_concurrent_retirement_and_reentrant_upload(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_query_cache.cpp").read_text()
        streamer = (
            "class CounterStreamer"
            + source.split("class CounterStreamer", 1)[1].split("} // namespace", 1)[0]
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-query-retirement-") as folder:
            cpp = Path(folder) / "queries.cpp"
            binary = Path(folder) / "queries"
            cpp.write_text(DRIVER.replace("__STREAMER__", streamer))
            result = subprocess.run(
                ["clang++", "-std=c++20", "-pthread", str(cpp), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
