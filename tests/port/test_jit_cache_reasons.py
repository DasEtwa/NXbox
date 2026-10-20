# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production invalidation decisions and window counters with host stubs."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INTERFACE = ROOT / "src/dynarmic/src/dynarmic/backend/x64/a64_interface.cpp"


def method(name, path=INTERFACE):
    source = path.read_text()
    match = re.search(
        rf"^    (?:static )?void {name}\(.*?^    \}}", source, re.MULTILINE | re.DOTALL
    )
    if not match:
        raise AssertionError(name)
    return match.group()


class JitCacheReasonTests(unittest.TestCase):
    def run_cpp(self, program, profile=1):
        with tempfile.TemporaryDirectory(prefix="nxbox-jit-reasons-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(program)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    f"-DNXBOX_STALL_PROFILE={profile}",
                    f"-I{ROOT / 'src'}",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_window_deltas_include_zero_duration_flushes(self):
        self.run_cpp(r"""
#include <cassert>
#include <thread>
#include "common/nxbox_stall.h"
int main() {
    using namespace NxboxStall;
    auto work = [] {
        for (unsigned i = 0; i < 10000; ++i) {
            Record(Kind::JitFlush, 0);
            AddJit(JitEvent::CapacityClears);
        }
    };
    std::thread a(work), b(work);
    a.join(); b.join();
    const auto window = Take(Kind::JitFlush);
    assert(window.total_us == 0 && window.max_us == 0 && window.calls == 20000);
    assert(TakeJit(JitEvent::CapacityClears) == 20000);
    assert(Take(Kind::JitFlush).calls == 0);
    assert(TakeJit(JitEvent::CapacityClears) == 0);
    SampleJitCache(0, 123, 192, true);
    SampleJitCache(0, 124, 192, true);
    SampleJitCache(0, 4, 192); // A clear changes usage, not the high-water mark.
    auto& cache = JitCaches()[0];
    assert(cache.used == 4 && cache.peak == 124 && cache.capacity == 192);
    assert(cache.compiled.exchange(0) == 2 && cache.compiled == 0);
}
""")

    def test_requested_invalidation_and_capacity_are_distinct(self):
        program = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <utility>
#include "common/nxbox_stall.h"
using u64 = std::uint64_t;
using u32 = std::uint32_t;
enum class HaltReason { None, CacheInvalidation };
bool Has(HaltReason hr, HaltReason bit) { return hr == bit; }
namespace boost::icl {
template<class T> struct discrete_interval {
    static auto closed(T start, T end) { return std::pair{start, end}; }
};
}
struct Ranges {
    std::set<std::pair<u64, u64>> values;
    void add(std::pair<u64, u64> range) { values.insert(range); }
    bool empty() const { return values.empty(); }
    void clear() { values.clear(); }
};
struct Harness {
    static constexpr size_t MiB = 1024 * 1024;
    struct Code {
        size_t capacity = 128 * MiB;
        size_t used = 130748812 + 4 * MiB;
        size_t SpaceRemaining() { return capacity > used ? capacity - used : 0; }
        void ClearCache() { used = 4 * MiB; }
        void EnsureMemoryCommitted(size_t n) { assert(SpaceRemaining() >= n); }
    } block_of_code;
    struct Emitter {
        unsigned clears = 0, ranges = 0;
        void ClearCache() { ++clears; }
        void InvalidateCacheRanges(const Ranges& r) { assert(!r.empty()); ++ranges; }
    } emitter;
    struct State { void ResetRSB() {} } jit_state;
    bool invalidate_entire_cache = false;
    Ranges invalid_cache_ranges;
    std::set<unsigned> restored_records;
    std::mutex invalidation_mutex;
    HaltReason halt = HaltReason::None;
    void HaltExecution(HaltReason hr) { halt = hr; }
    void ClearHalt(HaltReason) { halt = HaltReason::None; }
    void SampleCache() {}
METHODS
    void CapacityMiss() {
CAPACITY
    }
};
int main() {
    using namespace NxboxStall;
    Harness h;
    h.InvalidateCacheRange(0, 0);
    assert(h.halt == HaltReason::None && h.invalid_cache_ranges.empty());
    h.InvalidateCacheRange(0x1000, 64);
    h.InvalidateCacheRange(0x2000, 4);
    assert(h.emitter.ranges == 0 && h.emitter.clears == 0);
    h.PerformRequestedCacheInvalidation(h.halt);
    assert(h.emitter.ranges == 1 && h.emitter.clears == 0);
    assert(h.invalid_cache_ranges.empty());
    h.PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
    assert(h.emitter.ranges == 1); // A halt alone cannot discard code.
    h.ClearCache(); h.ClearCache(); // Requests coalesce; reason counts applied clears.
    h.PerformRequestedCacheInvalidation(h.halt);
    assert(h.emitter.clears == 1);
    h.block_of_code.used = 130748812 + 4 * Harness::MiB;
    h.CapacityMiss();
    assert(h.emitter.clears == 2);
    h.block_of_code.capacity = 192 * Harness::MiB;
    h.block_of_code.used = 130748812 + 4 * Harness::MiB;
    for (unsigned i = 0; i < 100; ++i) h.CapacityMiss();
    assert(h.emitter.clears == 2); // The same working set no longer evacuates.
    // Self-modifying code must still invalidate even with the larger capacity.
    h.InvalidateCacheRange(0x1000, 64);
    h.PerformRequestedCacheInvalidation(h.halt);
    assert(h.emitter.ranges == 2);
#if NXBOX_STALL_PROFILE
    assert(TakeJit(JitEvent::RangeCalls) == 3);
    assert(TakeJit(JitEvent::RangeBytes) == 132);
    assert(TakeJit(JitEvent::RangeBatches) == 2);
    assert(TakeJit(JitEvent::ClearRequests) == 2);
    assert(TakeJit(JitEvent::CacheClears) == 2);
    assert(TakeJit(JitEvent::ExplicitClears) == 1);
    assert(TakeJit(JitEvent::CapacityClears) == 1);
    assert(Take(Kind::JitFlush).calls == 1);
    assert(TakeJit(JitEvent::CapacityClears) == 0);
#endif
}
"""
        for backend in (INTERFACE, INTERFACE.with_name("a32_interface.cpp")):
            source = backend.read_text()
            capacity = re.search(
                r"        constexpr size_t MINIMUM_REMAINING_CODESIZE.*?"
                r"block_of_code.EnsureMemoryCommitted\(MINIMUM_REMAINING_CODESIZE\);",
                source,
                re.DOTALL,
            )[0]
            methods = "\n".join(
                method(name, backend)
                for name in (
                    "ClearCache",
                    "InvalidateCacheRange",
                    "PerformRequestedCacheInvalidation",
                )
            )
            actual = program.replace("METHODS", methods).replace("CAPACITY", capacity)
            for profile in (0, 1):
                with self.subTest(backend=backend.name, profile=profile):
                    self.run_cpp(actual, profile)

    def test_sample_reaches_keys_after_startup_history_fills(self):
        program = r"""
#include <cassert>
#include <cstdint>
#include <unordered_set>
#include "common/nxbox_stall.h"
using u64 = std::uint64_t;
struct History {
    static constexpr size_t HISTORY_LIMIT = 65536;
    std::unordered_set<u64> sampled_keys;
METHODS
};
int main() {
    using namespace NxboxStall;
    History history;
    // Entirely after the prefix history's first 65k addresses; FPCR variants too.
    auto compile = [&] {
        for (u64 pc = 0x100000; pc < 0x200000; pc += 4) {
            history.RecordSampledCompilation(pc);
            history.RecordSampledCompilation(pc | (1ULL << 58));
        }
    };
    compile();
    auto sampled = TakeJit(JitEvent::SampledNewKey);
    assert(sampled > 7000 && sampled < 9500);
    assert(TakeJit(JitEvent::SampledRepeatKey) == 0);
    compile(); // History survives cache eviction: the same keys are now repeats.
    assert(TakeJit(JitEvent::SampledNewKey) == 0);
    assert(TakeJit(JitEvent::SampledRepeatKey) == sampled);
    assert(TakeJit(JitEvent::SampledUnknownKey) == 0);
}
""".replace("METHODS", method("RecordCompilation") + method("RecordSampledCompilation"))
        self.run_cpp(program)

    def test_every_cache_event_is_exported_once(self):
        header = (ROOT / "src/common/nxbox_stall.h").read_text()
        events = re.search(r"enum class JitEvent \{(.*?)    Count", header, re.DOTALL)[1]
        events = re.findall(r"    (\w+),", events)
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        exported = re.findall(r'\{NxboxStall::JitEvent::(\w+), "\w+"\}', session)
        self.assertCountEqual(events, exported)
        size = re.search(r"const char\*>, (\d+)> jit_events", session)[1]
        self.assertEqual(int(size), len(exported))
        source = INTERFACE.read_text()
        self.assertIn("JitEvent::DiskStale", source)
        fastmem = (INTERFACE.parent / "emit_x64_memory.cpp.inc").read_text()
        self.assertIn("JitEvent::FastmemFaults", fastmem)
        self.assertIn("JitEvent::FastmemInvalidations", fastmem)


if __name__ == "__main__":
    unittest.main()
