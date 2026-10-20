# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production CPU dirty tracking and the draw-time upload scan cache."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "src/video_core/buffer_cache"


def without_includes(path):
    return "\n".join(
        line
        for line in path.read_text().splitlines()
        if not line.startswith(("#include", "#pragma once"))
    )


class BufferUploadRangeCacheTests(unittest.TestCase):
    def test_production_tracking_matches_uncached_uploads(self):
        # Compile the real tracker and word manager, substituting only platform types
        # and the device's page-protection callback. No GPU, Boost or SDK required.
        driver = r"""
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <deque>
#include <cstdlib>
#include <string_view>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>
#include "common/alignment.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "video_core/buffer_cache/upload_range_cache.h"
namespace Core { constexpr u64 DEVICE_PAGESIZE = 4096; }
namespace Common { template<class T> using unordered_set = std::unordered_set<T>; }
__WORD_MANAGER__
__MEMORY_TRACKER__
struct Device {
    void UpdatePagesCachedBatch(std::span<const std::pair<VAddr, size_t>>, int) {}
};
using Tracker = VideoCommon::MemoryTrackerBase<Device>;
using Ranges = std::vector<std::pair<u64, u64>>;
enum class BufferUploadSource { Other };
namespace NxboxUpload { enum class Event { BufferUpload }; void Record(Event) {} }
struct BufferCopy { u64 src_offset, dst_offset, size;
                    bool operator==(const BufferCopy&) const = default; };
struct TestBuffer { u64 cpu_addr_cached = 0; };
struct LegacyRuntime {};
struct CachedRuntime { bool BindingResolutionCacheEnabled() const { return true; } };
unsigned scans = 0;
struct TestMemory {
    Tracker& tracker;
    bool IsUploadRangeCached(u64 address, u64 size) const {
        return tracker.IsUploadRangeCached(address, size);
    }
    template<class F> void ForEachUploadRange(u64 address, u64 size, F&& function, bool enabled) {
        ++scans;
        tracker.ForEachUploadRange(address, size, std::forward<F>(function), enabled);
    }
};
template<class P> struct BufferCache {
    using Buffer = TestBuffer;
    P runtime;
    TestMemory memory_tracker;
    std::vector<BufferCopy> upload_copies;
    std::vector<BufferCopy> uploaded;
    bool any_buffer_uploaded = false;
    struct GpuRanges {
        template<class F> void ForEachInRange(u64, u64, F&&) {}
    } gpu_modified_ranges;
    explicit BufferCache(Tracker& tracker) : memory_tracker{tracker} {}
    void UploadMemory(Buffer&, u64, u64, std::span<BufferCopy> copies) {
        uploaded.assign(copies.begin(), copies.end());
    }
    bool SynchronizeBuffer(Buffer&, DAddr, u32, BufferUploadSource);
};
__SYNCHRONIZE_BUFFER__
template<class P> Ranges UploadWithRuntime(Tracker& tracker, u64 address, u64 size) {
    BufferCache<P> cache{tracker};
    TestBuffer buffer;
    cache.SynchronizeBuffer(buffer, address, static_cast<u32>(size), BufferUploadSource::Other);
    Ranges result;
    for (const auto& copy : cache.uploaded) result.emplace_back(copy.dst_offset, copy.size);
    return result;
}
Ranges Upload(Tracker& tracker, u64 address, u64 size, bool enabled) {
    return enabled ? UploadWithRuntime<CachedRuntime>(tracker, address, size)
                   : UploadWithRuntime<LegacyRuntime>(tracker, address, size);
}
int main() {
    Device device;
    auto old = std::make_unique<Tracker>(device);
    auto fast = std::make_unique<Tracker>(device);
    constexpr u64 region = 1ULL << 22;
    constexpr u64 page = 4096;
    if (const char* value = std::getenv("NXBOX_BUFFER_UPLOAD_RANGE_CACHE");
        value && std::string_view{value} == "0") {
        assert(Upload(*old, region, page, false) == Upload(*fast, region, page, true));
        assert(!fast->IsUploadRangeCached(region, page));
        old->MarkRegionAsCpuModified(region + 1, 1);
        fast->MarkRegionAsCpuModified(region + 1, 1);
        assert(Upload(*old, region, page, false) == Upload(*fast, region, page, true));
        assert(!fast->IsUploadRangeCached(region, page));
        return 0;
    }
    const auto sync = [&](u64 address, u64 size) {
        assert(Upload(*old, address, size, false) == Upload(*fast, address, size, true));
    };
    sync(region - page, page * 2); // Cross a tracking region boundary.
    assert(fast->IsUploadRangeCached(region - page, page * 2));
    old->MarkRegionAsCpuModified(region + 1, 1);
    fast->MarkRegionAsCpuModified(region + 1, 1);
    assert(!fast->IsUploadRangeCached(region - page, page * 2));
    sync(region - page, page * 2);
    old->MarkRegionAsCpuModified(region * 3, 1);
    fast->MarkRegionAsCpuModified(region * 3, 1);
    assert(fast->IsUploadRangeCached(region - page, page * 2)); // Unrelated writes retain it.
    sync(region - page, page * 2);
    // Cache a clean range while a cached store is pending; flush must invalidate it again.
    for (bool ranged : {false, true}) {
        old->CachedCpuWrite(region + 2, 1);
        fast->CachedCpuWrite(region + 2, 1);
        sync(region, page);
        assert(fast->IsUploadRangeCached(region, page));
        if (ranged) {
            old->FlushCachedWrites(region + 9, 1);
            fast->FlushCachedWrites(region + 9, 1);
        } else {
            old->FlushCachedWrites();
            fast->FlushCachedWrites();
        }
        assert(!fast->IsUploadRangeCached(region, page));
        sync(region, page);
    }
    std::mt19937 random(0xB0FF3);
    unsigned hits = 0;
    for (unsigned step = 0; step < 20000; ++step) {
        const u64 address = (random() % 4096) * page + random() % page;
        const u64 size = 1 + random() % (32 * page);
        switch (random() % 7) {
        case 0:
            old->MarkRegionAsCpuModified(address, size);
            fast->MarkRegionAsCpuModified(address, size);
            break;
        case 1:
            old->CachedCpuWrite(address, size);
            fast->CachedCpuWrite(address, size);
            break;
        case 2:
            old->FlushCachedWrites(); fast->FlushCachedWrites();
            break;
        case 3:
            old->FlushCachedWrites(address, size); fast->FlushCachedWrites(address, size);
            break;
        case 4: // Storage writes preserve CPU ownership rules.
            old->MarkRegionAsGpuModified(address, size);
            fast->MarkRegionAsGpuModified(address, size);
            break;
        }
        sync(address, size);
        assert(fast->IsUploadRangeCached(address, size));
        const unsigned before = scans;
        sync(address, size);
        assert(scans == before + 1); // Only the legacy path re-scans the unchanged range.
        ++hits;
        // Return to a stable uniform/index/vertex range between changing ranges.
        sync(region, page);
    }
    assert(hits == 20000);
    // Empty, overflowing and out-of-aperture requests must never create cache hits.
    VideoCommon::UploadRangeCache cache;
    for (const auto [address, size] : Ranges{{0, 0}, {~u64{0}, 4}, {(1ULL << 34) - 1, 2}}) {
        cache.Record(address, size);
        cache.Invalidate(address, size);
        assert(!cache.Contains(address, size));
    }
    // Hash collisions must compare the exact range, including sub-page offsets.
    for (u64 i = 0; i < 1024; ++i) cache.Record(i * page, page);
    for (u64 i = 0; i < 1024; ++i) assert(!cache.Contains(i * page + 1, page));
}
"""
        driver = driver.replace(
            "__WORD_MANAGER__", without_includes(CACHE / "word_manager.h")
        ).replace("__MEMORY_TRACKER__", without_includes(CACHE / "memory_tracker_base.h"))
        sync_source = (CACHE / "buffer_cache.h").read_text()
        sync = sync_source.split("bool BufferCache<P>::SynchronizeBuffer(", 1)[1].split(
            "\ntemplate <class P>", 1
        )[0]
        driver = driver.replace(
            "__SYNCHRONIZE_BUFFER__",
            "template<class P>\nbool BufferCache<P>::SynchronizeBuffer(" + sync,
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-upload-ranges-") as folder:
            source = Path(folder) / "tracker.cpp"
            binary = Path(folder) / "tracker"
            source.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O1",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run(
                [str(binary)],
                check=True,
                timeout=30,
                env={**os.environ, "NXBOX_BUFFER_UPLOAD_RANGE_CACHE": "1"},
            )
            subprocess.run(
                [str(binary)],
                check=True,
                timeout=30,
                env={**os.environ, "NXBOX_BUFFER_UPLOAD_RANGE_CACHE": "0"},
            )


if __name__ == "__main__":
    unittest.main()
