# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production upload methods with synthetic renderer/memory mocks."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]


class UniformUploadTest(unittest.TestCase):
    def test_uploads(self):
        source = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        methods = source[source.index("template <class P>\nu64 BufferCache<P>::UploadMemory") :]
        methods = methods[: methods.index("template <class P>\nbool BufferCache<P>::InlineMemory")]
        methods = methods.replace("template <class P>", "template <bool Mapped>")
        methods = methods.replace("BufferCache<P>", "Harness<Mapped>")
        harness = r'''
#include <cassert>
#include <iostream>
#include <random>
#include "video_core/buffer_cache/uniform_upload_cache.h"
using u8 = std::uint8_t;
using u64 = std::uint64_t;
using DAddr = u64;
struct BufferCopy { u64 src_offset, dst_offset, size; };
struct Buffer {
    u64 CpuAddr() const { return 4096; }
    void ImmediateUpload(u64, std::span<const u8>) { ++uploads; }
    unsigned uploads{};
};
struct Memory {
    std::array<u8, 262144> bytes{};
    void ReadBlockUnsafe(u64 addr, u8* out, u64 size) {
        std::copy_n(bytes.data() + addr - 4096, size, out);
    }
    template <typename T> T* GetPointer(u64 addr) {
        return reinterpret_cast<T*>(bytes.data() + addr - 4096);
    }
};
void WatchEvent(Memory&, const char*, u64, u64, u64) {}
struct Runtime {
    struct Staging { std::span<u8> mapped_span; u64 offset; Buffer buffer; };
    std::array<u8, 262144> bytes{};
    std::vector<BufferCopy> last;
    unsigned uploads{};
    Staging UploadStagingBuffer(u64 size) { return {std::span{bytes}.first(size), 256, {}}; }
    bool CanReorderUpload(Buffer&, std::span<BufferCopy>) { return false; }
    void CopyBuffer(Buffer&, Buffer&, std::span<BufferCopy> copies, bool, bool) {
        assert(!copies.empty());
        ++uploads;
        last.assign(copies.begin(), copies.end());
    }
};
template <bool Mapped> struct Harness {
    static constexpr bool USE_MEMORY_MAPS = Mapped;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = Mapped;
    Memory device_memory;
    Runtime runtime;
    VideoCommon::UniformUploadCache uniform_upload_cache;
    std::vector<u8> scratch;
    // Exercise both direct pointers and the non-granular scratch path.
    bool IsRangeGranular(u64 address, u64) { return address == 4096; }
    std::span<u8> ImmediateBuffer(u64 size) { scratch.resize(size); return scratch; }
    u64 UploadMemory(Buffer&, u64, u64, std::span<BufferCopy>, bool);
    u64 ImmediateUploadMemory(Buffer&, u64, std::span<const BufferCopy>, bool);
    u64 MappedUploadMemory(Buffer&, u64, std::span<BufferCopy>, bool);
};
'''
        checks = r'''
template <bool Mapped> void CheckUploads() {
    Harness<Mapped> h;
    Buffer b;
    auto upload = [&](bool deduplicate = true) {
        std::array copies{BufferCopy{0, 0, 4096}, BufferCopy{4096, 8192, 4096}};
        return h.UploadMemory(b, 8192, 4096, copies, deduplicate);
    };
    assert(upload() == 8192);
    assert(upload() == 0);
    h.device_memory.bytes[8192] = 1;
    assert(upload() == 4096);
    if constexpr (Mapped) {
        assert(h.runtime.last.size() == 1);
        assert(h.runtime.last[0].dst_offset == 8192);
        assert(h.runtime.last[0].src_offset == 4096 + 256);
    }
    assert(upload() == 0);
    // Identical CPU bytes must be uploaded again after GPU/DMA/inline/delete invalidation.
    h.uniform_upload_cache.Invalidate(4096 + 8192 + 1, 1);
    assert(upload() == 4096);
    assert(upload(false) == 8192);
    assert(upload() == 8192);
    assert(upload() == 0);
}
int main() {
    using VideoCommon::UniformUploadCache;
    assert(UniformUploadCache::Enabled(0x01007EF00011E000ULL, nullptr));
    assert(!UniformUploadCache::Enabled(0x01007EF00011E000ULL, "0"));
    assert(!UniformUploadCache::Enabled(0, nullptr));
    assert(!UniformUploadCache::Enabled(1, "1"));
    CheckUploads<true>();
    CheckUploads<false>();
    UniformUploadCache cache;
    std::array<u8, 64> data{};
    assert(!cache.MatchAndRecord(1024, data));
    cache.Invalidate(1088, 64); // Adjacent ranges do not overlap.
    assert(cache.MatchAndRecord(1024, data));
    assert(!cache.MatchAndRecord(1056, data));
    assert(!cache.MatchAndRecord(1024, data));
    std::vector<u8> large(65537);
    assert(!cache.MatchAndRecord(1024, large));
    assert(!cache.MatchAndRecord(1024, large));
    assert(!cache.MatchAndRecord(1024, data));
    assert(!cache.MatchAndRecord(0, {}));
    assert(!cache.MatchAndRecord(UINT64_MAX - 63, data));
    cache.Invalidate(UINT64_MAX, 1);
    assert(!cache.MatchAndRecord(UINT64_MAX - 63, data));
    UniformUploadCache eviction;
    for (unsigned i = 0; i < 17; ++i) {
        assert(!eviction.MatchAndRecord(i * 128, data));
    }
    assert(!eviction.MatchAndRecord(0, data));
    assert(eviction.MatchAndRecord(16 * 128, data));
    // Differential oracle: no cache hit may disagree with the simulated GPU bytes.
    std::array<u8, 8192> gpu{};
    std::mt19937 rng(0x12345678);
    for (unsigned i = 0; i < 20000; ++i) {
        const unsigned start = rng() % (gpu.size() - data.size());
        if (rng() % 3 == 0) {
            cache.Invalidate(start, data.size());
            std::fill_n(gpu.begin() + start, data.size(), u8(rng()));
        } else {
            data.fill(u8(rng() % 4));
            if (cache.MatchAndRecord(start, data)) {
                assert(std::equal(data.begin(), data.end(), gpu.begin() + start));
            } else {
                std::copy(data.begin(), data.end(), gpu.begin() + start);
            }
        }
    }
    std::cout << "PASS: title gate, exact equality, overlap/eviction, 20000 differential operations, mapped/immediate uploads and staging offsets\n";
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(harness + methods + checks)
            subprocess.run(
                ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(ROOT / "src"),
                 str(cpp), "-o", str(binary)], check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
