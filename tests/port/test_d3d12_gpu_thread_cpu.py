# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise GPU-thread cache semantics without the Windows SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


def function(source, signature):
    start = source.index(signature)
    return source[start : source.index("\n}", start) + 2]


class GpuThreadCpuTests(unittest.TestCase):
    def compile_run(self, driver):
        with tempfile.TemporaryDirectory(prefix="nxbox-gpu-cpu-") as directory:
            source = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            source.write_text(driver)
            build = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
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
            self.assertEqual(build.returncode, 0, build.stderr)
            subprocess.run([str(binary)], check=True, timeout=15)

    def test_seeded_hash_matches_uncached_across_changes_and_oversize(self):
        self.compile_run(r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
int main() {
    D3D12::Pso::ByteHashCache<128> cache;
    std::array<unsigned char, 256> bytes{};
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        bytes[iteration % bytes.size()] ^= iteration;
        for (const auto size : {0u, 1u, 127u, 128u, 129u, 256u}) {
            for (const auto seed : {0ull, 1ull, 0xcbf29ce484222325ull}) {
                const auto expected = D3D12::Pso::HashBytes(bytes.data(), size, seed);
                assert(cache.Get(bytes.data(), size, seed) == expected);
                assert(cache.Get(bytes.data(), size, seed) == expected);
            }
        }
    }
}
""")

    def test_changed_bytes_with_same_size_and_seed_never_return_the_stale_hash(self):
        self.compile_run(r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
int main() {
    D3D12::Pso::ByteHashCache<16> cache;
    std::array<unsigned char, 16> bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const auto stale = cache.Get(bytes.data(), bytes.size(), 7);
    assert(stale == D3D12::Pso::HashBytes(bytes.data(), bytes.size(), 7));
    bytes[9] ^= 0x40;
    const auto fresh = D3D12::Pso::HashBytes(bytes.data(), bytes.size(), 7);
    assert(fresh != stale); // otherwise a stale answer would be indistinguishable
    assert(cache.Get(bytes.data(), bytes.size(), 7) == fresh);
}
""")

    def test_uniform_replacement_preserves_lru_and_releases_old_copy_without_allocating(self):
        self.compile_run(r"""
#include <cassert>
#include <cstdlib>
#include <new>
#include "video_core/renderer_d3d12/d3d12_upload_cache.h"
static bool track;
static unsigned allocations;
void* operator new(std::size_t size) {
    if (track) ++allocations;
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc{};
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
struct Resource { int value; };
int main() {
    D3D12::UniformCopyCache<Resource, 2> cache;
    auto a = std::make_shared<Resource>();
    auto b = std::make_shared<Resource>();
    auto c = std::make_shared<Resource>();
    auto old_copy = std::make_shared<Resource>();
    auto new_copy = std::make_shared<Resource>();
    std::weak_ptr<Resource> old_lease = old_copy;
    cache.Insert(a, 1, 0, 64, old_copy);
    old_copy.reset();
    cache.Insert(b, 1, 0, 64, new_copy);
    track = true;
    cache.Insert(a, 2, 0, 64, new_copy, 256);
    track = false;
    assert(allocations == 0 && old_lease.expired());
    assert(!cache.Find(a, 1, 0, 64));
    std::uint32_t offset{};
    assert(cache.Find(a, 2, 0, 64, &offset) == new_copy && offset == 256);
    cache.Insert(c, 1, 0, 64, new_copy);
    assert(!cache.Find(b, 1, 0, 64));
    assert(cache.Find(a, 2, 0, 64));
    assert(!cache.Find(a, 2, 1, 64));
    cache.Clear();
    assert(!cache.Find(a, 2, 0, 64));
}
""")

    def test_pool_cache_preserves_budget_identity_expiry_and_cross_thread_sharing(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        pool = function(source, "std::shared_ptr<BufferHeapPool> BufferPool(")
        self.compile_run(
            r"""
#include <cassert>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
struct MemoryBudget {};
struct BufferHeapPool {};
__POOL__
int main() {
    auto a = std::make_shared<MemoryBudget>();
    auto b = std::make_shared<MemoryBudget>();
    auto first = BufferPool(a);
    assert(BufferPool(a) == first);
    auto second = BufferPool(b);
    assert(second != first);
    assert(BufferPool(a) == first);
    std::thread worker([&] { assert(BufferPool(a) == first); });
    worker.join();
    std::weak_ptr<BufferHeapPool> expired = first;
    first.reset();
    assert(expired.expired());
    auto recreated = BufferPool(a);
    assert(recreated && BufferPool(a) == recreated);
    // Keep a pool alive after destroying its budget; new ownership must never hit it.
    a.reset();
    auto next = std::make_shared<MemoryBudget>();
    assert(BufferPool(next) != recreated);
}
""".replace("__POOL__", pool)
        )

    def test_retention_opens_recording_before_testing_ids(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        retain = function(source, "void Context::Retain(")
        self.compile_run(
            r"""
#include <cassert>
#include <memory>
#include <unordered_set>
#include <vector>
struct Resource {};
using ResourcePtr = std::shared_ptr<Resource>;
struct Context {
    bool recording{};
    unsigned opened{};
    std::vector<ResourcePtr> retained;
    std::unordered_set<Resource*> retained_ids;
    void Commands() { ++opened; recording = true; retained.clear(); retained_ids.clear(); }
    void Retain(const ResourcePtr&);
};
__RETAIN__
int main() {
    Context context;
    auto a = std::make_shared<Resource>();
    auto b = std::make_shared<Resource>();
    context.Retain(a);
    context.Retain(b);
    context.Retain(a);
    assert(context.opened == 1 && context.retained.size() == 2 && a.use_count() == 2);
    context.recording = false;
    context.Retain(a);
    assert(context.opened == 2 && context.retained.size() == 1 && a.use_count() == 2);
    context.Retain({});
    assert(context.opened == 2 && context.retained.size() == 1);
}
""".replace("__RETAIN__", retain)
        )

    def test_borrowed_image_handle_keeps_ownership_only_when_copied(self):
        header = (NATIVE / "d3d12_texture_cache.h").read_text()
        begin = header.index("    const ResourcePtr& BorrowImageHandle()")
        method = header[begin : header.index("\n    }", begin) + 6]
        self.compile_run(
            r"""
#include <cassert>
#include <memory>
#include <type_traits>
#include <utility>
struct Resource {};
using ResourcePtr = std::shared_ptr<Resource>;
struct ImageView {
    ResourcePtr resource;
__BORROW__
};
int main() {
    ImageView view{std::make_shared<Resource>()};
    static_assert(std::is_same_v<decltype(view.BorrowImageHandle()), const ResourcePtr&>);
    const auto& borrowed = view.BorrowImageHandle();
    assert(borrowed && view.resource.use_count() == 1);
    ResourcePtr retained = borrowed;
    assert(view.resource.use_count() == 2);
    view.resource.reset();
    assert(!borrowed && retained && retained.use_count() == 1);
}
""".replace("__BORROW__", method)
        )

    def test_diagnostics_and_specialization_cache_are_wired(self):
        source = (NATIVE / "d3d12_context.cpp").read_text()
        commands = function(source, "ID3D12GraphicsCommandList* Context::Commands()")
        self.assertLess(
            commands.index("if (debug_recorder)"), commands.index("GetCurrentThreadId()")
        )
        info = function(source, "void LogDeviceInfoQueue(")
        self.assertIn('static const bool debug = BisectEnabled("NXBOX_D3D12_DEBUG")', info)
        self.assertNotIn("GetEnvironmentVariable", info)
        pipeline = (NATIVE / "d3d12_pipeline_cache.cpp").read_text()
        self.assertRegex(
            pipeline, r"vertex_specialization_hash.Get\(\s*regs.vertex_attrib_format.data\(\)"
        )


if __name__ == "__main__":
    unittest.main()
