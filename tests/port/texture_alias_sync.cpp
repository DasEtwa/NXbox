// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>
using u32 = unsigned;
using u64 = unsigned long long;
using ImageId = unsigned;
using ImageViewId = unsigned;
namespace boost::container {
template <class T, size_t>
using small_vector = std::vector<T>;
}
namespace fmt {
template <class... T>
std::string format(const char*, T...) {
    return {};
}
} // namespace fmt
namespace PassDump {
struct Config {
    bool enabled = false;
    u64 frame = 0;
};
Config GetConfig() {
    return {};
}
} // namespace PassDump
namespace Settings {
struct {
    struct {
        bool active = false;
    } resolution_info;
} values;
} // namespace Settings
enum ImageFlagBits { CpuModified = 1, GpuModified = 2, Tracked = 4, Rescaled = 8 };
bool True(unsigned value) {
    return value != 0;
}
bool False(unsigned value) {
    return value == 0;
}
struct Copy {
    struct {
        unsigned width = 1, height = 1;
    } extent;
};
struct AliasedImage {
    ImageId id;
    std::vector<Copy> copies{{}};
};
struct Aliases : std::vector<AliasedImage> {
    mutable unsigned scans = 0;
    auto begin() const {
        ++scans;
        return std::vector<AliasedImage>::begin();
    }
    auto begin() {
        ++scans;
        return std::vector<AliasedImage>::begin();
    }
};
struct ImageBase {
    unsigned flags = Tracked;
    struct {
        struct {
            unsigned width = 64, height = 64;
        } size;
    } info;
    u64 alias_sync_frame_tick = std::numeric_limits<u64>::max();
    u64 alias_sync_generation = 0, modification_tick = 0;
    unsigned lru_index = 0, gpu_addr = 0, payload = 0;
    Aliases aliased_images;
};
using Image = ImageBase;
struct ImageViewBase {
    ImageId image_id = 0;
    bool buffer = false;
    u64 prepare_frame_tick = std::numeric_limits<u64>::max();
    bool IsBuffer() const {
        return buffer;
    }
};
template <class P>
struct TextureCache {
    bool texture_cache_fastpath = true;
    // ALIAS_SWITCH
    u64 alias_sync_generation = 1, frame_tick = 0, modification_tick = 0;
    std::array<Image, 5> slot_images{};
    std::array<ImageViewBase, 6> slot_image_views{};
    unsigned prepares = 0, refreshes = 0;
    std::vector<std::pair<unsigned, unsigned>> copies;
    struct {
        unsigned touches = 0;
        void Touch(unsigned, u64) {
            ++touches;
        }
    } lru_cache;
    TextureCache() {
        for (unsigned i = 1; i < 5; ++i)
            slot_image_views[i].image_id = i;
        slot_image_views[5].image_id = 1; // Two descriptors share the image synchronization.
        for (unsigned i = 1; i < 3; ++i) {
            slot_images[i].aliased_images.push_back({i + 1});
            slot_images[i + 1].aliased_images.push_back({i});
        }
    }
    void TrackImage(Image& image, ImageId) {
        image.flags |= Tracked;
    }
    void UntrackImage(Image& image, ImageId) {
        image.flags &= ~Tracked;
    }
    void RefreshContents(Image& image, ImageId) {
        ++prepares;
        if (image.flags & CpuModified) {
            ++refreshes;
            image.flags &= ~CpuModified;
            image.flags |= Tracked;
            image.alias_sync_frame_tick = std::numeric_limits<u64>::max();
        }
    }
    bool ImageCanRescale(Image&) {
        return false;
    }
    void ScaleUp(Image& image) {
        if (!(image.flags & Rescaled)) {
            image.flags |= Rescaled;
            ++alias_sync_generation;
        }
    }
    void ScaleDown(Image& image) {
        if (image.flags & Rescaled) {
            image.flags &= ~Rescaled;
            ++alias_sync_generation;
        }
    }
    void CopyImage(ImageId dst, ImageId src, const std::vector<Copy>&) {
        copies.emplace_back(dst, src);
        slot_images[dst].payload = slot_images[src].payload;
    }
    u64 CaptureFrame() {
        return frame_tick + 1;
    }
    void ReportPassDump(const std::string&) {}
    void NotePassDumpImageWrite(ImageId, const char*, unsigned) {}
    void InvalidateAliasSync(ImageBase&) noexcept;
    void MarkModification(ImageBase&) noexcept;
    void SynchronizeAliases(ImageId);
    void PrepareImage(ImageId, bool, bool);
    void PrepareImageView(ImageViewId, bool, bool);
};
// METHODS
int main() {
    TextureCache<int> fast;
    fast.texture_cache_fastpath = false; // New paths work without the older opt-in.
    const bool enabled = fast.alias_sync_fastpath;
    setenv("NXBOX_TEXTURE_ALIAS_SYNC_FASTPATH", "0", 1);
    TextureCache<int> reference;
    setenv("NXBOX_TEXTURE_ALIAS_SYNC_FASTPATH", enabled ? "1" : "0", 1);
    reference.texture_cache_fastpath = false;
    fast.PrepareImageView(1, false, false);
    const auto cold_prepares = fast.prepares;
    const auto cold_scans = fast.slot_images[1].aliased_images.scans;
    for (int i = 0; i < 100; ++i) {
        fast.PrepareImageView(1, false, false);
        fast.PrepareImageView(5, false, false);
    }
    assert(fast.prepares == cold_prepares + (enabled ? 0 : 200));
    assert(fast.slot_images[1].aliased_images.scans == cold_scans + (enabled ? 0 : 200));
    // A source already written before alias creation must bypass the cached read.
    for (auto* cache : {&fast, &reference}) {
        cache->slot_images[4].payload = 42;
        cache->MarkModification(cache->slot_images[4]);
        cache->slot_images[1].aliased_images.push_back({4});
        cache->slot_images[4].aliased_images.push_back({1});
        ++cache->alias_sync_generation;
        cache->PrepareImageView(1, false, false);
    }
    assert(fast.slot_images[1].payload == 42);
    assert(fast.copies == reference.copies);
    std::mt19937 random{0x915f};
    for (unsigned step = 0; step < 20000; ++step) {
        const auto id = 1 + random() % 4;
        const auto action = random() % 10;
        if (action < 3) {
            for (auto* cache : {&fast, &reference}) {
                cache->slot_images[id].payload = step;
                cache->MarkModification(cache->slot_images[id]);
            }
        } else if (action == 3) {
            ++fast.frame_tick;
            ++reference.frame_tick;
        } else if (action == 4) {
            fast.ScaleUp(fast.slot_images[id]);
            reference.ScaleUp(reference.slot_images[id]);
        } else if (action == 5) {
            fast.slot_images[id].flags |= CpuModified;
            reference.slot_images[id].flags |= CpuModified;
        } else if (action == 6) {
            fast.slot_images[id].flags &= ~Tracked;
            reference.slot_images[id].flags &= ~Tracked;
        }
        const bool modification = random() % 3 == 0;
        const bool invalidate = random() % 11 == 0;
        fast.PrepareImageView(id, modification, invalidate);
        reference.PrepareImageView(id, modification, invalidate);
        assert(fast.copies == reference.copies);
        assert(fast.modification_tick == reference.modification_tick);
        assert(fast.refreshes == reference.refreshes);
        for (unsigned i = 1; i < 5; ++i) {
            assert(fast.slot_images[i].payload == reference.slot_images[i].payload);
            assert(fast.slot_images[i].flags == reference.slot_images[i].flags);
            assert(fast.slot_images[i].modification_tick ==
                   reference.slot_images[i].modification_tick);
        }
    }
}
