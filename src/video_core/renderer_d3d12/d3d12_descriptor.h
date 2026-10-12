// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace D3D12 {
// A recording owns immutable slots only until its submission tick changes.
// Full semantic keys are compared; hash collisions cannot alias different tables.
template <class Value>
class RecordingTableCache {
public:
    std::optional<Value> Find(std::uint64_t current, std::string_view key) {
        Refresh(current);
        const auto found = tables.find(std::string{key});
        return found == tables.end() ? std::nullopt : std::optional<Value>{found->second};
    }
    void Remember(std::uint64_t current, std::string key, Value value) {
        Refresh(current);
        tables.insert_or_assign(std::move(key), std::move(value));
    }

private:
    void Refresh(std::uint64_t current) {
        if (tick != current) {
            tables.clear();
            tick = current;
        }
    }
    std::uint64_t tick{};
    std::unordered_map<std::string, Value> tables;
};
} // namespace D3D12

#ifdef _WIN32
#include <d3d12.h>

#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"

namespace Tegra::Texture {
struct TSCEntry;
}
namespace D3D12 {
class ImageView;
class Sampler;
class Framebuffer;

// ---- Buffer cache bindings (filled by BufferCacheRuntime hooks, consumed per draw) ----
struct BoundBuffer {
    ResourcePtr resource; // null: null buffer, a null descriptor is written
    u32 offset{};
    u32 size{};
    bool written{};
    u32 pixel_format{}; // VideoCore::Surface::PixelFormat for texel buffers
};
struct StageBufferBindings {
    std::vector<BoundBuffer> uniform;
    std::vector<BoundBuffer> storage;
    std::vector<BoundBuffer> texel; // texture buffers first, then image buffers
    void Clear() {
        uniform.clear();
        storage.clear();
        texel.clear();
    }
};
struct BoundVertexBuffer {
    ResourcePtr resource;
    u64 offset{};
    u64 size{};
    u32 stride{};
};
struct GeometryBindings {
    static constexpr size_t MaxVertexBuffers = 32;
    std::array<BoundVertexBuffer, MaxVertexBuffers> vertex;
    ResourcePtr index;
    u64 index_offset{};
    u64 index_size{};
    bool index_is_32bit{};
    bool index_unsupported{}; // u8 indices / indexed quad conversion: draw is skipped
};
struct BindingState {
    static constexpr size_t NumStages = 5;
    std::array<StageBufferBindings, NumStages> stages;
    GeometryBindings geometry;
    size_t current_stage{};
};

// ---- Pure helpers (also used by the rasterizer) ----
DXGI_FORMAT DsvFormat(DXGI_FORMAT view_format);
D3D12_SAMPLER_DESC MakeSamplerDesc(const Tegra::Texture::TSCEntry& entry);

struct DescriptorTable {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    u32 count{};
    u32 stride{};
    D3D12_CPU_DESCRIPTOR_HANDLE Slot(u32 index) const {
        return {cpu.ptr + static_cast<SIZE_T>(index) * stride};
    }
    // Sub-range of a larger allocation (one ring allocation feeds the three view tables).
    DescriptorTable Slice(u32 first, u32 n) const {
        return {Slot(first), {gpu.ptr + static_cast<UINT64>(first) * stride}, n, stride};
    }
};

// Shader-visible CBV/SRV/UAV and sampler ring heaps plus CPU-only RTV/DSV rings. Every
// allocation is tagged with Context::CurrentTick() and recycled once that tick completes.
// When a ring is full the allocator submits the open command list and/or waits on the oldest
// tick; callers must therefore allocate BEFORE recording per-draw state.
class D3D12DescriptorAllocator {
public:
    static constexpr u32 ViewHeapSize = 1u << 18;
    static constexpr u32 SamplerHeapSize = 2048; // D3D12 limit for shader-visible samplers
    static constexpr u32 RtvHeapSize = 4096;
    static constexpr u32 DsvHeapSize = 512;

    explicit D3D12DescriptorAllocator(Context& context, u32 view_capacity = ViewHeapSize,
                                      u32 sampler_capacity = SamplerHeapSize);

    // Rebind for every draw: helpers can replace heaps within the same command list.
    // Reserve the whole draw/dispatch before allocating from any ring. Reclaiming
    // another heap must never submit tables already reserved for this operation.
    void Reserve(u32 view_count, u32 sampler_count, u32 rtv_count = 0, u32 dsv_count = 0);
    void SetHeaps(ID3D12GraphicsCommandList* commands);
    DescriptorTable AllocateViews(u32 count);
    DescriptorTable AllocateSamplers(u32 count);
    D3D12_CPU_DESCRIPTOR_HANDLE AllocateRtv(u32 count);
    D3D12_CPU_DESCRIPTOR_HANDLE AllocateDsv();

    // Writers. Null variants keep unbound/unsupported slots valid.
    void WriteNullCbv(const DescriptorTable& table, u32 slot);
    void WriteNullSrv(const DescriptorTable& table, u32 slot, D3D12_SRV_DIMENSION dimension);
    void WriteNullUav(const DescriptorTable& table, u32 slot);
    void WriteNullSampler(const DescriptorTable& table, u32 slot);
    void WriteCbv(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    void WriteStorageUav(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    void WriteTexelSrv(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    void WriteTexelUav(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    // Returns false (and writes a null view) when the view has no backing image.
    bool WriteImageSrv(const DescriptorTable& table, u32 slot, const ImageView& view,
                       D3D12_SRV_DIMENSION fallback_dimension, const ResourcePtr& replacement = {});
    void WriteSampler(const DescriptorTable& table, u32 slot, const Sampler& sampler);

    bool WriteImageUav(const DescriptorTable& table, u32 slot, const ImageView& view,
                       DXGI_FORMAT format);
    // Copy an immutable CPU descriptor into a fence-retired shader-visible slot.
    void WriteUavDescriptor(const DescriptorTable& table, u32 slot, const ResourcePtr& resource,
                            const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc);
    // Semantic keys must include resource identity, ranges, format and swizzle.
    // Tables are reusable only within one recording tick, never across retirement.
    std::optional<DescriptorTable> FindTable(std::string_view key, bool sampler = false);
    void RememberTable(std::string key, DescriptorTable table, bool sampler = false);
    ID3D12DescriptorHeap* ResourceHeap() const {
        return views.heap.Get();
    }
    ID3D12DescriptorHeap* SamplerHeap() const {
        return samplers.heap.Get();
    }

    // Render target views derived from an image view's subresource range.
    // nullptr (or a view without image) creates a null RTV that discards writes.
    void CreateRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ImageView* view);
    void CreateDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ImageView& view,
                   D3D12_DSV_FLAGS flags = D3D12_DSV_FLAG_NONE);

private:
    struct Ring {
        ComPtr<ID3D12DescriptorHeap> heap;
        RingAllocator math;
        u32 stride{};
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        bool shader_visible{};
        Ring(u32 capacity) : math{capacity} {}
    };
    void Init(Ring& ring, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity, bool visible);
    DescriptorTable Allocate(Ring& ring, u32 count);

    struct DescriptorKey {
        std::array<u8, 64> description{};
        std::uintptr_t resource{};
        u32 kind{}, size{};
        bool operator==(const DescriptorKey&) const = default;
    };
    struct DescriptorKeyHash {
        size_t operator()(const DescriptorKey& key) const noexcept {
            const std::string_view bytes{reinterpret_cast<const char*>(key.description.data()),
                                         key.size};
            return std::hash<std::string_view>{}(bytes) ^
                   std::hash<std::uintptr_t>{}(key.resource) ^ (static_cast<size_t>(key.kind) << 1);
        }
    };
    struct CachedDescriptor {
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        ResourcePtr resource; // Prevent address reuse while the descriptor is cached.
    };
    struct DescriptorCache {
        ComPtr<ID3D12DescriptorHeap> heap;
        std::unordered_map<DescriptorKey, CachedDescriptor, DescriptorKeyHash> entries;
        u32 cursor{}, stride{}, capacity{};
    };
    void InitCache(DescriptorCache& cache, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity);
    void RefreshCaches();
    void WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE type, const ResourcePtr& resource, u32 kind,
                     const void* desc, size_t size, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    u64 cache_tick{};
    DescriptorCache view_cache, sampler_cache;
    std::unordered_map<SIZE_T, DescriptorKey> slot_contents;
    std::array<RecordingTableCache<DescriptorTable>, 2> table_cache;
    Context& context;
    Ring views{ViewHeapSize};
    Ring samplers{SamplerHeapSize};
    Ring rtvs{RtvHeapSize};
    Ring dsvs{DsvHeapSize};
};
} // namespace D3D12

#endif // _WIN32
