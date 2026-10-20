// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <vector>
using GPUVAddr = unsigned long long;
using DAddr = unsigned long long;
using ImageId = unsigned;
namespace boost::container {
template <class T, unsigned N>
using small_vector = std::vector<T>;
}
enum class ImageType { Linear, Tiled };
enum class ImageFlagBits { Remapped = 1, Strong = 2 };
enum class RelaxedOptions { None = 0, ForceBrokenViews = 1, Size = 2, Format = 4 };
constexpr int operator&(int flags, ImageFlagBits bit) {
    return flags & static_cast<int>(bit);
}
constexpr int operator&(RelaxedOptions flags, RelaxedOptions bit) {
    return static_cast<int>(flags) & static_cast<int>(bit);
}
bool True(int value) {
    return value != 0;
}
bool False(int value) {
    return value == 0;
}
struct ImageInfo {
    ImageType type = ImageType::Tiled;
    int format = 1;
    unsigned pitch = 32;
    int size = 64;
    bool operator==(const ImageInfo&) const = default;
};
bool SameImageLookupInfo(const ImageInfo& lhs, const ImageInfo& rhs) {
    return lhs == rhs;
}
struct ImageBase {
    ImageInfo info;
    int flags = 0;
    GPUVAddr gpu_addr = 256;
    int modification_tick = 0;
};
bool IsPitchLinearSameSize(const ImageInfo& lhs, const ImageInfo& rhs, bool strict) {
    return !strict || lhs.size == rhs.size;
}
bool IsViewCompatible(int, int, bool, bool) {
    return true;
}
bool IsSubresource(const ImageInfo&, const ImageBase& image, GPUVAddr address, RelaxedOptions, bool,
                   bool) {
    return address == image.gpu_addr;
}
unsigned CalculateGuestSizeInBytes(const ImageInfo& info) {
    return info.size;
}
struct Memory {
    int translations = 0;
    bool mapped = true;
    DAddr offset = 0;
    std::optional<DAddr> GpuToCpuAddress(GPUVAddr address, unsigned = 0) {
        ++translations;
        return mapped ? std::optional<DAddr>{address + offset} : std::nullopt;
    }
};
template <class P>
struct TextureCache {
    struct CachedImageLookup {
        ImageInfo info;
        GPUVAddr address = 0;
        DAddr cpu_address = 0;
        RelaxedOptions options{};
        const Memory* memory = nullptr;
        ImageId id{};
    };
    std::array<CachedImageLookup, 64> cached_image_lookup{};
    Memory memory;
    Memory* gpu_memory = &memory;
    bool texture_cache_fastpath = true;
    std::array<ImageBase, 4> slot_images{};
    struct Runtime {
        bool HasBrokenTextureViewFormats() const {
            return true;
        }
        bool HasNativeBgr() const {
            return true;
        }
    } runtime;
    std::vector<ImageId> region{1};
    int walks = 0;
    template <class F>
    void ForEachImageInRegion(DAddr, unsigned, F&& callback) {
        ++walks;
        for (ImageId id : region)
            if (callback(id, slot_images[id]))
                break;
    }
    ImageId FindImage(const ImageInfo&, GPUVAddr, RelaxedOptions);
};
// PRODUCTION_METHOD
int main() {
    constexpr auto strict = RelaxedOptions::None;
    TextureCache<int> cache;
    ImageInfo info;
    assert(cache.FindImage(info, 256, strict) == 1);
    for (int i = 0; i < 100; ++i)
        assert(cache.FindImage(info, 256, strict) == 1);
    assert(cache.walks == 1 && cache.memory.translations == 101);
    cache.texture_cache_fastpath = false;
    assert(cache.FindImage(info, 256, strict) == 1);
    assert(cache.walks == 2 && cache.memory.translations == 102);
    cache.texture_cache_fastpath = true;
    info.size = 128; // A different query cannot reuse an overlap decision.
    assert(cache.FindImage(info, 256, strict) == 1);
    assert(cache.walks == 3);
    Memory other_memory;
    cache.gpu_memory = &other_memory;
    cache.FindImage(info, 256, strict);
    assert(other_memory.translations == 1 && cache.walks == 4);
    other_memory.offset = 4096;
    cache.FindImage(info, 256, strict);
    assert(cache.walks == 5);       // Replacing physical backing also invalidates a query hit.
    cache.slot_images[1].flags = 1; // Remapped images must not be returned.
    assert(cache.FindImage(info, 256, strict) == 0);
    cache.slot_images[1].flags = 0;
    cache.cached_image_lookup = {};
    cache.region = {1, 2};
    cache.slot_images[1].info.format = 2;
    cache.slot_images[1].modification_tick = 20;
    cache.slot_images[2].modification_tick = 10;
    const int before_multiple = cache.walks;
    // The first compatible alias can be newer than the exact-format image.
    assert(cache.FindImage(info, 256, strict) == 1);
    cache.slot_images[2].modification_tick = 30;
    assert(cache.FindImage(info, 256, strict) == 2);
    assert(cache.walks == before_multiple + 2);
    const int before_relaxed = cache.walks;
    cache.FindImage(info, 256, RelaxedOptions::Format);
    cache.FindImage(info, 256, RelaxedOptions::Format);
    assert(cache.walks == before_relaxed + 2);
    cache.cached_image_lookup = {};
    cache.region = {1};
    cache.slot_images[1].info = info;
    cache.slot_images[1].info.type = ImageType::Linear;
    info.type = ImageType::Linear;
    info.size = 256;
    assert(cache.FindImage(info, 256, strict) == 1); // Weak pitch-linear view allows size mismatch.
    cache.slot_images[1].flags = 2;
    assert(cache.FindImage(info, 256, strict) == 0); // Strong now requires exact size.
    cache.cached_image_lookup = {};
    other_memory.mapped = false;
    assert(cache.FindImage(info, 256, strict) == 0);
}
