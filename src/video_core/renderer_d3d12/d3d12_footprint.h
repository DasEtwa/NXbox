// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

#include "video_core/renderer_d3d12/d3d12_formats.h"

namespace D3D12 {
constexpr std::uint64_t CheckedAdd(std::uint64_t a, std::uint64_t b) {
    if (a > UINT64_MAX - b)
        throw std::overflow_error("D3D12 footprint addition");
    return a + b;
}
constexpr std::uint64_t CheckedMultiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > UINT64_MAX / b)
        throw std::overflow_error("D3D12 footprint multiplication");
    return a * b;
}
constexpr std::uint64_t Align(std::uint64_t value, std::uint64_t alignment) {
    if (!alignment || (alignment & (alignment - 1)))
        throw std::invalid_argument("D3D12 alignment must be a power of two");
    return CheckedAdd(value, alignment - 1) & ~(alignment - 1);
}
struct CopyFootprint {
    std::uint64_t offset;
    std::uint32_t row_pitch;
    std::uint32_t rows;
    std::uint64_t row_bytes;
    std::uint64_t slice_pitch;
    std::uint64_t size;
    constexpr std::uint64_t End() const {
        return CheckedAdd(offset, size);
    }
};
// Conservative allocation includes the final row's padding. Actual texture layouts
// are obtained from GetCopyableFootprints; this also handles partial copy extents.
constexpr CopyFootprint MakeFootprint(FormatInfo format, std::uint32_t width, std::uint32_t height,
                                      std::uint32_t depth = 1, std::uint64_t base = 0) {
    if (!format.Supported() || !format.block_width || !format.block_height ||
        !format.bytes_per_block || !width || !height || !depth)
        throw std::invalid_argument("D3D12 invalid copy footprint");
    const auto blocks = (std::uint64_t{width} + format.block_width - 1) / format.block_width;
    const auto rows = (std::uint64_t{height} + format.block_height - 1) / format.block_height;
    const auto row_bytes = CheckedMultiply(blocks, format.bytes_per_block);
    const auto pitch = Align(row_bytes, 256);
    if (pitch > UINT32_MAX)
        throw std::overflow_error("D3D12 row pitch");
    const auto slice = CheckedMultiply(pitch, rows);
    return {Align(base, 512),
            static_cast<std::uint32_t>(pitch),
            static_cast<std::uint32_t>(rows),
            row_bytes,
            slice,
            CheckedMultiply(slice, depth)};
}
// Plan GPU row copies with the same tight/padded layout as CPU repacking. Validate
// both ranges before invoking the visitor so a bad extent cannot partially copy.
template <typename Visitor>
void ForEachRowCopy(std::uint64_t destination_size, std::uint64_t source_size,
                    std::uint64_t destination_offset, std::uint64_t source_offset,
                    std::uint64_t row_bytes, std::uint32_t rows, std::uint32_t depth,
                    std::uint64_t destination_pitch, std::uint64_t destination_slice,
                    std::uint64_t source_pitch, std::uint64_t source_slice, Visitor&& visitor) {
    if (!row_bytes || !rows || !depth || destination_pitch < row_bytes ||
        source_pitch < row_bytes || destination_slice < CheckedMultiply(destination_pitch, rows) ||
        source_slice < CheckedMultiply(source_pitch, rows))
        throw std::invalid_argument("D3D12 invalid GPU row layout");
    const auto required = [=](std::uint64_t offset, std::uint64_t pitch, std::uint64_t slice) {
        return CheckedAdd(offset,
                          CheckedAdd(CheckedMultiply(slice, depth - 1),
                                     CheckedAdd(CheckedMultiply(pitch, rows - 1), row_bytes)));
    };
    if (required(destination_offset, destination_pitch, destination_slice) > destination_size ||
        required(source_offset, source_pitch, source_slice) > source_size)
        throw std::out_of_range("D3D12 GPU row copy bounds");
    for (std::uint32_t z = 0; z < depth; ++z)
        for (std::uint32_t y = 0; y < rows; ++y)
            visitor(destination_offset + z * destination_slice + y * destination_pitch,
                    source_offset + z * source_slice + y * source_pitch, row_bytes);
}
// Repack only meaningful bytes; padding is neither sourced nor copied back to the guest.
inline void RepackRows(std::span<std::uint8_t> destination, std::span<const std::uint8_t> source,
                       std::uint64_t row_bytes, std::uint32_t rows, std::uint32_t depth,
                       std::uint64_t destination_pitch, std::uint64_t destination_slice,
                       std::uint64_t source_pitch, std::uint64_t source_slice) {
    if (!row_bytes || !rows || !depth || destination_pitch < row_bytes ||
        source_pitch < row_bytes || destination_slice < CheckedMultiply(destination_pitch, rows) ||
        source_slice < CheckedMultiply(source_pitch, rows))
        throw std::invalid_argument("D3D12 invalid row layout");
    const auto required = [=](std::uint64_t pitch, std::uint64_t slice) {
        return CheckedAdd(CheckedMultiply(slice, depth - 1),
                          CheckedAdd(CheckedMultiply(pitch, rows - 1), row_bytes));
    };
    if (required(destination_pitch, destination_slice) > destination.size() ||
        required(source_pitch, source_slice) > source.size())
        throw std::out_of_range("D3D12 row repack bounds");
    for (std::uint32_t z = 0; z < depth; ++z)
        for (std::uint32_t y = 0; y < rows; ++y)
            std::memcpy(destination.data() + z * destination_slice + y * destination_pitch,
                        source.data() + z * source_slice + y * source_pitch, row_bytes);
}
} // namespace D3D12
