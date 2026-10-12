# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute T7 heap placement and pressure policy without Windows or game data."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"

DRIVER = r"""
#include <cassert>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;
using SIZE_T = size_t;
using D3D12_HEAP_TYPE = int;
using D3D12_RESOURCE_STATES = int;
constexpr int D3D12_HEAP_TYPE_DEFAULT=0, D3D12_HEAP_TYPE_UPLOAD=1, D3D12_HEAP_TYPE_READBACK=2;
constexpr u64 D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT=65536;
constexpr int D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS=1;
#define IID_PPV_ARGS(x) x
namespace D3D12 {
template<class T> struct ComPtr {
    std::shared_ptr<T> value;
    T* Get() const { return value.get(); }
    T* operator->() const { return value.get(); }
    explicit operator bool() const { return bool(value); }
};
struct ID3D12Heap {};
struct FakeResource {
    std::vector<u8> bytes;
    int Map(int, const void*, void** out) { *out=bytes.data(); return 0; }
};
struct D3D12_RESOURCE_DESC { u64 Width; };
struct D3D12_RANGE { SIZE_T Begin, End; };
struct D3D12_HEAP_DESC {
    u64 SizeInBytes{}, Alignment{};
    struct { int Type{}; int CreationNodeMask{}, VisibleNodeMask{}; } Properties;
    int Flags{};
};
struct ID3D12Device {
    int heaps=0;
    u64 last_offset=0;
    struct Allocation { u64 SizeInBytes; };
    Allocation GetResourceAllocationInfo(int, int, const D3D12_RESOURCE_DESC* desc) {
        return {(desc->Width+65535)&~u64{65535}};
    }
    int CreateHeap(const D3D12_HEAP_DESC*, ComPtr<ID3D12Heap>* out) {
        ++heaps; out->value=std::make_shared<ID3D12Heap>(); return 0;
    }
    int CreatePlacedResource(ID3D12Heap*, u64 offset, const D3D12_RESOURCE_DESC* desc,
                             int, void*, ComPtr<FakeResource>* out) {
        last_offset=offset;
        out->value=std::make_shared<FakeResource>(); out->value->bytes.resize(desc->Width);
        return 0;
    }
};
void Check(int result, const char*, ID3D12Device*) { if(result) throw std::runtime_error("fake failure"); }
struct MemoryBudget { std::atomic<u64> used{}; };
struct Resource {
    ComPtr<FakeResource> handle;
    int heap{}, state{};
    u64 allocation_size{};
    std::span<u8> mapped;
};
using ResourcePtr=std::shared_ptr<Resource>;
__POOL__
}
int main() {
    using namespace D3D12;
    constexpr u64 mib=1ULL<<20;
    // Observed Xbox limit is 5 GiB, not the Series X retail game partition.
    assert(BufferMemoryTarget(5120*mib, 3200*mib, 512*mib)==2176*mib);
    assert(BufferMemoryTarget(5120*mib, 5100*mib, 512*mib)==512*mib);
    assert(BufferMemoryTarget(5120*mib, 6000*mib, 512*mib)==512*mib);
    assert(BufferMemoryTarget(16*mib, 16*mib, 0)==0);
    BufferHeapSlots slots(8);
    assert(!slots.Allocate(0) && !slots.Allocate(9));
    assert(slots.Allocate(3)==0 && slots.Allocate(2)==3 && slots.Allocate(3)==5);
    assert(!slots.Allocate(1));
    slots.Release(3,2); assert(!slots.Allocate(3));
    slots.Release(0,3); assert(slots.Allocate(5)==0);
    slots.Release(0,5); slots.Release(5,3); assert(slots.Used()==0);
    ID3D12Device device;
    auto budget=std::make_shared<MemoryBudget>();
    auto pool=BufferPool(budget);
    auto allocate=[&](u64 size, int type=0) {
        return pool->Allocate(&device,budget,{size},type,0,pool);
    };
    auto first=allocate(256);
    auto second=allocate(256);
    assert(device.heaps==1 && device.last_offset==65536);
    assert(budget->used==16*mib);
    auto fence=first;
    first.reset();
    auto third=allocate(256); assert(device.last_offset==131072);
    // A submitted fence's shared lease prevents reuse, even after cache eviction.
    pool->Trim(); assert(pool->pages.size()==1);
    fence.reset(); auto recycled=allocate(256); assert(device.last_offset==0);
    auto upload=allocate(256,D3D12_HEAP_TYPE_UPLOAD); auto readback=allocate(256,D3D12_HEAP_TYPE_READBACK);
    assert(upload->mapped.size()==256 && readback->mapped.size()==256);
    assert(device.heaps==3 && budget->used==48*mib);
    second.reset(); third.reset(); recycled.reset(); upload.reset(); readback.reset();
    pool->Trim(); assert(pool->pages.empty() && budget->used==0);
    assert(!allocate(17*mib)); // Dedicated committed fallback, no page leak.
    auto next=allocate(256); assert(device.last_offset==0);
    auto same=BufferPool(budget); assert(same==pool);
    auto other=BufferPool(std::make_shared<MemoryBudget>()); assert(other!=pool);
}
"""


class T7BuffersTests(unittest.TestCase):
    def test_heap_recycling_fence_lifetime_and_budget(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        pool = context.split("struct BufferHeapPage {", 1)[1].split(
            "} // namespace\nResourcePtr Context::CreateResource", 1
        )[0]
        source_text = DRIVER.replace("__POOL__", "struct BufferHeapPage {" + pool)
        with tempfile.TemporaryDirectory(prefix="nxbox-t7-") as folder:
            source = Path(folder) / "buffers.cpp"
            binary = Path(folder) / "buffers"
            source.write_text(source_text)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
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
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_pressure_is_not_a_hard_allocation_failure(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        self.assertNotIn("exceeds 512 MiB budget", context)
        allocation = context.split("ResourcePtr Context::CreateResource", 1)[1]
        self.assertIn("NativeBufferMemoryTarget", allocation)
        self.assertIn("result == E_OUTOFMEMORY", allocation)
        self.assertIn("pool->Trim()", allocation)

    def test_uniform_padding_and_vertex_range_contract(self):
        source = (BACKEND / "d3d12_buffer_cache.cpp").read_text()
        self.assertIn("upload.Request(65536)", source)
        self.assertIn("staging.mapped_span.end(), u8{0}", source)
        self.assertIn("size > width - offset", source)
        self.assertIn("offset & 255", source)
        self.assertIn("size != padded", source)
        self.assertIn("{std::move(buffer), offset, size, stride}", source)
        self.assertIn("Snapshot every source range", source)
