// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace D3D12 {
// A placed footprint may reuse the caller's lease only when every row/slice
// matches and its absolute GPU offset satisfies TEXTURE_DATA_PLACEMENT_ALIGNMENT.
constexpr bool CanDirectTextureUpload(std::uint64_t offset, std::uint64_t row_stride,
                                      std::uint64_t slice_stride, std::uint64_t row_pitch,
                                      std::uint64_t slice_pitch) {
    return offset % 512 == 0 && row_stride == row_pitch && slice_stride == slice_pitch;
}
constexpr bool IsExactBlit(std::int32_t sx0, std::int32_t sy0, std::int32_t sx1, std::int32_t sy1,
                           std::int32_t dx0, std::int32_t dy0, std::int32_t dx1, std::int32_t dy1) {
    return sx0 >= 0 && sy0 >= 0 && dx0 >= 0 && dy0 >= 0 && sx1 > sx0 && sy1 > sy0 && dx1 > dx0 &&
           dy1 > dy0 && sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0;
}
constexpr bool CoversSingleSubresource(std::int32_t x0, std::int32_t y0, std::int32_t x1,
                                       std::int32_t y1, std::uint32_t width, std::uint32_t height,
                                       std::uint32_t depth, std::int32_t levels,
                                       std::int32_t layers) {
    return x0 == 0 && y0 == 0 && x1 > 0 && y1 > 0 && static_cast<std::uint32_t>(x1) == width &&
           static_cast<std::uint32_t>(y1) == height && depth == 1 && levels == 1 && layers == 1;
}
// Bounded, allocation-free five-second histogram. The largest repeated upload
// wins; unique large allocations must not hide a recurring framebuffer upload.
class RepeatedTextureUploads {
public:
    struct Entry {
        std::uint64_t address{}, size{}, count{};
        std::uint32_t format{}, width{}, height{};
        std::string_view reason;
        bool gpu_modified{};
    };
    void Add(Entry entry) {
        Entry* smallest = &entries.front();
        for (auto& item : entries) {
            if (item.count && item.address == entry.address && item.size == entry.size &&
                item.format == entry.format) {
                entry.count = item.count + 1;
                item = entry;
                return;
            }
            if (item.size < smallest->size)
                smallest = &item;
        }
        if (!smallest->count || entry.size > smallest->size) {
            entry.count = 1;
            *smallest = entry;
        }
    }
    Entry LargestRepeated() const {
        Entry result{};
        for (const auto& item : entries)
            if (item.count > 1 && item.size > result.size)
                result = item;
        return result;
    }
    void Reset() {
        entries = {};
    }

private:
    std::array<Entry, 64> entries{};
};
} // namespace D3D12
