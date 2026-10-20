# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production submission policies and transport without the Windows SDK."""

from pathlib import Path
import unittest

from test_d3d12_depth_recording import function_body, run_cpp

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "src/video_core/renderer_d3d12"


class SubmissionPressureTests(unittest.TestCase):
    def test_default_and_legacy_queue_preserve_partial_submits_and_holds(self):
        run_cpp(r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_record_args.h"
struct Object {
    int refs = 1;
    void AddRef() { ++refs; }
    void Release() { --refs; }
};
int main() {
    using Stream = D3D12::RecordStream<>;
    static_assert(Stream::chunk_size == 256 * 1024 && Stream::chunk_count == 32);
    // A partial submission publishes one chunk. Fill the complete pool before
    // running the consumer: the default now absorbs 32 submissions, versus 8.
    for (size_t capacity : {size_t{8}, Stream::chunk_count}) {
        Stream stream{capacity};
        stream.metrics = true;
        Object object;
        size_t expected = 0;
        for (size_t round = 0; round < 4; ++round) {
            for (size_t i = 0; i < capacity; ++i) {
                D3D12::EncodeCall<[](void* target, size_t value, Object* hold) {
                    assert(value == (*static_cast<size_t*>(target))++);
                    assert(hold->refs > 1);
                }>(stream, round * capacity + i, &object);
                stream.Flush();
            }
            assert(object.refs == int(capacity + 1) && stream.waits == 0);
            for (size_t i = 0; i < capacity; ++i)
                assert(stream.Consume(&expected));
            assert(object.refs == 1 && expected == (round + 1) * capacity);
        }
        stream.Close();
        assert(!stream.Consume(&expected));
    }
    for (size_t invalid : {size_t{0}, size_t{1}, size_t{33}}) {
        bool rejected = false;
        try { Stream stream{invalid}; }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
}
""")

    def test_dma_deferral_keeps_cpu_reads_synchronous(self):
        source = (RENDERER / "d3d12_buffer_cache.h").read_text()
        methods = "\n".join(
            function_body(source, signature)
            for signature in ("void Finish()", "void FinishDmaCopy()")
        )
        run_cpp(
            r"""
#include <cassert>
#include <functional>
#include <string_view>
#include <vector>
enum class FrameCount { ForcedSubmitBufferDownload };
bool async_dma = false;
bool BisectEnabled(std::string_view flag) {
    assert(flag == "NXBOX_D3D12_ASYNC_DMA");
    return async_dma;
}
struct Context {
    int submits = 0, waits = 0;
    std::string_view site;
    std::vector<std::function<void()>> commands;
    int Submit(FrameCount) { return ++submits; }
    void Wait(int tick, std::string_view origin) {
        assert(tick == submits);
        ++waits; site = origin;
        for (auto& command : commands) command();
        commands.clear();
    }
    void Finish(FrameCount reason) { Wait(Submit(reason), "finish"); }
};
struct Runtime {
    Context context;
__METHODS__
};
int main() {
    for (bool enabled : {false, true}) {
        async_dma = enabled;
        Runtime runtime;
        int gpu_result = 0, readback = 0;
        // A GPU write, DMA and readback stay ordered even if DMA does not submit.
        runtime.context.commands.push_back([&] { gpu_result = 42; });
        runtime.FinishDmaCopy();
        assert(runtime.context.submits == int(!enabled));
        assert(runtime.context.waits == int(!enabled));
        if (!enabled) assert(runtime.context.site == "dma_copy");
        runtime.context.commands.push_back([&] { readback = gpu_result; });
        runtime.Finish(); // DownloadBufferMemory still calls the unconditional CPU wait.
        assert(readback == 42 && runtime.context.commands.empty());
        assert(runtime.context.waits == (enabled ? 1 : 2));
    }
}
""".replace("__METHODS__", methods)
        )
        shared = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        dma = function_body(shared, "bool BufferCache<P>::DMACopy")
        self.assertIn("runtime.FinishDmaCopy();", dma)
        self.assertIn("memory_tracker.MarkRegionAsGpuModified", dma)
        download = function_body(
            shared,
            "void BufferCache<P>::DownloadBufferMemory(Buffer& buffer, DAddr device_addr",
        )
        self.assertLess(download.index("runtime.Finish();"), download.index("WriteBlockUnsafe"))

    def test_large_clear_keeps_exact_bytes_without_per_mib_finishes(self):
        method = function_body(
            (RENDERER / "d3d12_buffer_cache.cpp").read_text(),
            "void BufferCacheRuntime::ClearBuffer",
        )
        run_cpp(
            r"""
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
using u8 = unsigned char; using u32 = unsigned; using u64 = unsigned long long;
struct Resource {
    std::vector<u8> bytes;
    explicit Resource(size_t size) : bytes(size, 0xa5) {}
    Resource* operator->() { return this; }
    Resource* handle = this;
    struct Desc { size_t Width; };
    Desc GetDesc() const { return {bytes.size()}; }
    int GetGPUVirtualAddress() const { return 0; }
};
using ResourcePtr = std::shared_ptr<Resource>;
struct Staging { ResourcePtr buffer; size_t offset; std::span<u8> mapped_span; };
struct Ring {
    Staging Request(size_t count) {
        auto resource = std::make_shared<Resource>(count);
        return {resource, 0, resource->bytes};
    }
};
namespace VideoCommon { struct BufferCopy { size_t src_offset, dst_offset, size; }; }
enum class FrameCount { BufferUploadBytes, OtherUploadBytes, ForcedSubmitBufferClear };
enum class FramePhase { BufferUpload };
struct FrameProfile {
    struct Scope { Scope(FrameProfile&, FramePhase) {} };
    void Count(FrameCount, size_t) {}
};
bool legacy = false;
bool BisectEnabled(std::string_view name) {
    assert(name == "NXBOX_D3D12_SYNC_BUFFER_CLEARS"); return legacy;
}
bool UploadTraceEnabled() { return false; }
namespace fmt { template<class... T> int format(const char*, T...) { return 0; } }
void Diagnostic(int) {}
void InvalidRange(int, const char*, u32, size_t, size_t) { assert(false); }
struct Context {
    FrameProfile profile;
    int finishes = 0;
    struct Copy { ResourcePtr dst, src; VideoCommon::BufferCopy region; };
    std::vector<Copy> copies;
    FrameProfile& Profile() { return profile; }
    int CurrentTick() { return 1; }
    void Replay() {
        for (auto& copy : copies)
            std::memcpy(copy.dst->bytes.data() + copy.region.dst_offset,
                        copy.src->bytes.data() + copy.region.src_offset, copy.region.size);
        copies.clear();
    }
    void Finish(FrameCount) { ++finishes; Replay(); }
};
struct BufferCacheRuntime {
    Context context;
    Ring upload;
    void CopyBuffer(ResourcePtr dst, ResourcePtr src,
                    std::span<const VideoCommon::BufferCopy> copies, bool) {
        for (const auto& copy : copies) context.copies.push_back({dst, src, copy});
    }
    void ClearBuffer(ResourcePtr, u32, size_t, u32);
};
__CLEAR__
int main() {
    constexpr size_t count = 3 * (1 << 20) + 12, offset = 8;
    for (bool sync : {false, true}) {
        legacy = sync;
        BufferCacheRuntime runtime;
        auto dst = std::make_shared<Resource>(count + 16);
        runtime.ClearBuffer(dst, offset, count, 0x12345678);
        assert(runtime.context.finishes == (sync ? 3 : 0));
        assert(runtime.context.copies.size() == (sync ? 1 : 4));
        runtime.context.Replay();
        for (size_t i = 0; i < dst->bytes.size(); ++i) {
            const u32 value = 0x12345678;
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            assert(dst->bytes[i] == ((i < offset || i >= offset + count)
                                        ? 0xa5 : bytes[(i - offset) % 4]));
        }
    }
}
""".replace("__CLEAR__", method)
        )
