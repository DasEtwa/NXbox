// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace D3D12 {
// DXGI numeric values keep this contract executable without a Windows SDK.
// Host storage geometry is independent of guest packed/block geometry.
enum class FormatConversion { None, PackedColor, Etc2, Eac, DepthStencil };
struct FormatInfo {
    std::string_view name;
    std::uint32_t resource_format;
    std::uint32_t view_format;
    std::uint32_t block_width;
    std::uint32_t block_height;
    std::uint32_t bytes_per_block;
    bool converted_astc;
    bool depth;
    std::uint32_t planes;
    FormatConversion conversion{FormatConversion::None};
    // Host component selected for guest R/G/B/A; 4 and 5 are zero and one.
    std::array<std::uint32_t, 4> components{0, 1, 2, 3};
    constexpr bool Supported() const {
        return resource_format != 0;
    }
    constexpr std::uint32_t RtvFormat() const {
        return depth || block_width != 1 ? 0u : view_format;
    }
    constexpr std::uint32_t DsvFormat() const {
        if (!depth)
            return 0;
        return resource_format == 19   ? 20u
               : resource_format == 44 ? 45u
               : resource_format == 39 ? 40u
               : resource_format == 53 ? 55u
                                       : 0u;
    }
    constexpr std::uint32_t SrvFormat(unsigned plane = 0) const {
        return plane && planes == 2 ? (resource_format == 19 ? 22u : 47u) : view_format;
    }
};
inline constexpr auto FormatTable = std::to_array<FormatInfo>({
    {"A8B8G8R8_UNORM", 27, 28, 1, 1, 4, false, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"A8B8G8R8_SNORM", 27, 31, 1, 1, 4, false, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_SNORM
    {"A8B8G8R8_SINT", 27, 32, 1, 1, 4, false, false, 1},  // R8G8B8A8_TYPELESS, R8G8B8A8_SINT
    {"A8B8G8R8_UINT", 27, 30, 1, 1, 4, false, false, 1},  // R8G8B8A8_TYPELESS, R8G8B8A8_UINT
    {"R5G6B5_UNORM", 85, 85, 1, 1, 2, false, false, 1},   // B5G6R5_UNORM, B5G6R5_UNORM
    {"B5G6R5_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::PackedColor},
    {"A1R5G5B5_UNORM", 86, 86, 1, 1, 2, false, false, 1}, // B5G5R5A1_UNORM, B5G5R5A1_UNORM
    {"A2B10G10R10_UNORM", 23, 24, 1, 1, 4, false, false,
     1}, // R10G10B10A2_TYPELESS, R10G10B10A2_UNORM
    {"A2B10G10R10_UINT", 23, 25, 1, 1, 4, false, false,
     1}, // R10G10B10A2_TYPELESS, R10G10B10A2_UINT
    {"A2R10G10B10_UNORM", 9, 11, 1, 1, 8, false, false, 1, FormatConversion::PackedColor},
    {"A1B5G5R5_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::PackedColor},
    {"A5B5G5R1_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::PackedColor},
    {"R8_UNORM", 60, 61, 1, 1, 1, false, false, 1}, // R8_TYPELESS, R8_UNORM
    {"R8_SNORM", 60, 63, 1, 1, 1, false, false, 1}, // R8_TYPELESS, R8_SNORM
    {"R8_SINT", 60, 64, 1, 1, 1, false, false, 1},  // R8_TYPELESS, R8_SINT
    {"R8_UINT", 60, 62, 1, 1, 1, false, false, 1},  // R8_TYPELESS, R8_UINT
    {"R16G16B16A16_FLOAT", 9, 10, 1, 1, 8, false, false,
     1}, // R16G16B16A16_TYPELESS, R16G16B16A16_FLOAT
    {"R16G16B16A16_UNORM", 9, 11, 1, 1, 8, false, false,
     1}, // R16G16B16A16_TYPELESS, R16G16B16A16_UNORM
    {"R16G16B16A16_SNORM", 9, 13, 1, 1, 8, false, false,
     1}, // R16G16B16A16_TYPELESS, R16G16B16A16_SNORM
    {"R16G16B16A16_SINT", 9, 14, 1, 1, 8, false, false,
     1}, // R16G16B16A16_TYPELESS, R16G16B16A16_SINT
    {"R16G16B16A16_UINT", 9, 12, 1, 1, 8, false, false,
     1}, // R16G16B16A16_TYPELESS, R16G16B16A16_UINT
    {"B10G11R11_FLOAT", 26, 26, 1, 1, 4, false, false, 1}, // R11G11B10_FLOAT, R11G11B10_FLOAT
    {"R32G32B32A32_UINT", 1, 3, 1, 1, 16, false, false,
     1}, // R32G32B32A32_TYPELESS, R32G32B32A32_UINT
    {"BC1_RGBA_UNORM", 70, 71, 4, 4, 8, false, false, 1},   // BC1_TYPELESS, BC1_UNORM
    {"BC2_UNORM", 73, 74, 4, 4, 16, false, false, 1},       // BC2_TYPELESS, BC2_UNORM
    {"BC3_UNORM", 76, 77, 4, 4, 16, false, false, 1},       // BC3_TYPELESS, BC3_UNORM
    {"BC4_UNORM", 79, 80, 4, 4, 8, false, false, 1},        // BC4_TYPELESS, BC4_UNORM
    {"BC4_SNORM", 79, 81, 4, 4, 8, false, false, 1},        // BC4_TYPELESS, BC4_SNORM
    {"BC5_UNORM", 82, 83, 4, 4, 16, false, false, 1},       // BC5_TYPELESS, BC5_UNORM
    {"BC5_SNORM", 82, 84, 4, 4, 16, false, false, 1},       // BC5_TYPELESS, BC5_SNORM
    {"BC7_UNORM", 97, 98, 4, 4, 16, false, false, 1},       // BC7_TYPELESS, BC7_UNORM
    {"BC6H_UFLOAT", 94, 95, 4, 4, 16, false, false, 1},     // BC6H_TYPELESS, BC6H_UF16
    {"BC6H_SFLOAT", 94, 96, 4, 4, 16, false, false, 1},     // BC6H_TYPELESS, BC6H_SF16
    {"ASTC_2D_4X4_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"B8G8R8A8_UNORM", 90, 87, 1, 1, 4, false, false, 1},   // B8G8R8A8_TYPELESS, B8G8R8A8_UNORM
    {"R32G32B32A32_FLOAT", 1, 2, 1, 1, 16, false, false,
     1}, // R32G32B32A32_TYPELESS, R32G32B32A32_FLOAT
    {"R32G32B32A32_SINT", 1, 4, 1, 1, 16, false, false,
     1},                                                // R32G32B32A32_TYPELESS, R32G32B32A32_SINT
    {"R32G32_FLOAT", 15, 16, 1, 1, 8, false, false, 1}, // R32G32_TYPELESS, R32G32_FLOAT
    {"R32G32_SINT", 15, 18, 1, 1, 8, false, false, 1},  // R32G32_TYPELESS, R32G32_SINT
    {"R32_FLOAT", 39, 41, 1, 1, 4, false, false, 1},    // R32_TYPELESS, R32_FLOAT
    {"R16_FLOAT", 53, 54, 1, 1, 2, false, false, 1},    // R16_TYPELESS, R16_FLOAT
    {"R16_UNORM", 53, 56, 1, 1, 2, false, false, 1},    // R16_TYPELESS, R16_UNORM
    {"R16_SNORM", 53, 58, 1, 1, 2, false, false, 1},    // R16_TYPELESS, R16_SNORM
    {"R16_UINT", 53, 57, 1, 1, 2, false, false, 1},     // R16_TYPELESS, R16_UINT
    {"R16_SINT", 53, 59, 1, 1, 2, false, false, 1},     // R16_TYPELESS, R16_SINT
    {"R16G16_UNORM", 33, 35, 1, 1, 4, false, false, 1}, // R16G16_TYPELESS, R16G16_UNORM
    {"R16G16_FLOAT", 33, 34, 1, 1, 4, false, false, 1}, // R16G16_TYPELESS, R16G16_FLOAT
    {"R16G16_UINT", 33, 36, 1, 1, 4, false, false, 1},  // R16G16_TYPELESS, R16G16_UINT
    {"R16G16_SINT", 33, 38, 1, 1, 4, false, false, 1},  // R16G16_TYPELESS, R16G16_SINT
    {"R16G16_SNORM", 33, 37, 1, 1, 4, false, false, 1}, // R16G16_TYPELESS, R16G16_SNORM
    {"R32G32B32_FLOAT", 5, 6, 1, 1, 12, false, false, 1}, // R32G32B32_TYPELESS, R32G32B32_FLOAT
    {"A8B8G8R8_SRGB", 27, 29, 1, 1, 4, false, false, 1},  // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"R8G8_UNORM", 48, 49, 1, 1, 2, false, false, 1},     // R8G8_TYPELESS, R8G8_UNORM
    {"R8G8_SNORM", 48, 51, 1, 1, 2, false, false, 1},     // R8G8_TYPELESS, R8G8_SNORM
    {"R8G8_SINT", 48, 52, 1, 1, 2, false, false, 1},      // R8G8_TYPELESS, R8G8_SINT
    {"R8G8_UINT", 48, 50, 1, 1, 2, false, false, 1},      // R8G8_TYPELESS, R8G8_UINT
    {"R32G32_UINT", 15, 17, 1, 1, 8, false, false, 1},    // R32G32_TYPELESS, R32G32_UINT
    {"R16G16B16X16_FLOAT",
     9,
     10,
     1,
     1,
     8,
     false,
     false,
     1,
     FormatConversion::None,
     {0, 1, 2, 5}},                                 // R16G16B16A16_TYPELESS, R16G16B16A16_FLOAT
    {"R32_UINT", 39, 42, 1, 1, 4, false, false, 1}, // R32_TYPELESS, R32_UINT
    {"R32_SINT", 39, 43, 1, 1, 4, false, false, 1}, // R32_TYPELESS, R32_SINT
    {"ASTC_2D_8X8_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_8X5_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_5X4_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"B8G8R8A8_SRGB", 90, 91, 1, 1, 4, false, false, 1}, // B8G8R8A8_TYPELESS, B8G8R8A8_UNORM_SRGB
    {"BC1_RGBA_SRGB", 70, 72, 4, 4, 8, false, false, 1}, // BC1_TYPELESS, BC1_UNORM_SRGB
    {"BC2_SRGB", 73, 75, 4, 4, 16, false, false, 1},     // BC2_TYPELESS, BC2_UNORM_SRGB
    {"BC3_SRGB", 76, 78, 4, 4, 16, false, false, 1},     // BC3_TYPELESS, BC3_UNORM_SRGB
    {"BC7_SRGB", 97, 99, 4, 4, 16, false, false, 1},     // BC7_TYPELESS, BC7_UNORM_SRGB
    {"A4B4G4R4_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::PackedColor},
    {"G4R4_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::PackedColor},
    {"ASTC_2D_4X4_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_8X8_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_8X5_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_5X4_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_5X5_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_5X5_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_10X8_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_10X8_SRGB", 27, 29, 1, 1, 4, true, false,
     1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_6X6_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_6X6_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_10X6_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_10X6_SRGB", 27, 29, 1, 1, 4, true, false,
     1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_10X5_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_10X5_SRGB", 27, 29, 1, 1, 4, true, false,
     1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_10X10_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_10X10_SRGB", 27, 29, 1, 1, 4, true, false,
     1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_12X10_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_12X10_SRGB", 27, 29, 1, 1, 4, true, false,
     1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_12X12_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_12X12_SRGB", 27, 29, 1, 1, 4, true, false,
     1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_8X6_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_8X6_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"ASTC_2D_6X5_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_6X5_SRGB", 27, 29, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB
    {"E5B9G9R9_FLOAT", 67, 67, 1, 1, 4, false, false, 1},  // R9G9B9E5_SHAREDEXP, R9G9B9E5_SHAREDEXP
    {"ETC2_RGB_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::Etc2},
    {"ETC2_RGBA_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::Etc2},
    {"ETC2_RGB_PTA_UNORM", 27, 28, 1, 1, 4, false, false, 1, FormatConversion::Etc2},
    {"ETC2_RGB_SRGB", 27, 29, 1, 1, 4, false, false, 1, FormatConversion::Etc2},
    {"ETC2_RGBA_SRGB", 27, 29, 1, 1, 4, false, false, 1, FormatConversion::Etc2},
    {"ETC2_RGB_PTA_SRGB", 27, 29, 1, 1, 4, false, false, 1, FormatConversion::Etc2},
    {"EAC_R11_UNORM", 53, 56, 1, 1, 2, false, false, 1, FormatConversion::Eac},
    {"EAC_R11_SNORM", 53, 58, 1, 1, 2, false, false, 1, FormatConversion::Eac},
    {"EAC_R11G11_UNORM", 33, 35, 1, 1, 4, false, false, 1, FormatConversion::Eac},
    {"EAC_R11G11_SNORM", 33, 37, 1, 1, 4, false, false, 1, FormatConversion::Eac},
    {"D32_FLOAT", 39, 41, 1, 1, 4, false, true, 1}, // R32_TYPELESS, R32_FLOAT
    {"D16_UNORM", 53, 56, 1, 1, 2, false, true, 1}, // R16_TYPELESS, R16_UNORM
    {"X8_D24_UNORM", 44, 46, 1, 1, 4, false, true, 2,
     FormatConversion::DepthStencil}, // R24G8_TYPELESS, R24_UNORM_X8_TYPELESS
    {"S8_UINT", 44, 46, 1, 1, 4, false, true, 2, FormatConversion::DepthStencil},
    {"D24_UNORM_S8_UINT", 44, 46, 1, 1, 4, false, true, 2,
     FormatConversion::DepthStencil}, // R24G8_TYPELESS, R24_UNORM_X8_TYPELESS
    {"S8_UINT_D24_UNORM", 44, 46, 1, 1, 4, false, true, 2, FormatConversion::DepthStencil},
    {"D32_FLOAT_S8_UINT", 19, 21, 1, 1, 8, false, true, 2,
     FormatConversion::DepthStencil}, // R32G8X24_TYPELESS, R32_FLOAT_X8X24_TYPELESS
});
constexpr FormatInfo LookupFormat(std::string_view name) {
    for (const auto& format : FormatTable) {
        if (format.name == name)
            return format;
    }
    return {name, 0, 0, 0, 0, 0, false, false, 0};
}

namespace FormatDetail {
// ETC2 RGB decoder adapted from etcdec v0.91, https://github.com/iOrange/etcdec.
// Local changes: inline linkage, defined sign extension, unaligned input loads.
/* MIT License


Copyright (c) 2022 Sergii Kudlai

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
/* http://graphics.stanford.edu/~seander/bithacks.html#VariableSignExtend */
inline int etcdec__extend_sign(int val, int bits) {
    const int sign = 1 << (bits - 1);
    return (val ^ sign) - sign;
}

inline int etcdec__clamp_255(int value) {
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}
inline int etcdec__clamp_2047(int value) {
    return value < 0 ? 0 : (value > 2047 ? 2047 : value);
}

inline unsigned long long etcdec__bswap(unsigned long long x) {
    return (((x) & 0xFF00000000000000ull) >> 56) | (((x) & 0x00FF000000000000ull) >> 40) |
           (((x) & 0x0000FF0000000000ull) >> 24) | (((x) & 0x000000FF00000000ull) >> 8) |
           (((x) & 0x00000000FF000000ull) << 8) | (((x) & 0x0000000000FF0000ull) << 24) |
           (((x) & 0x000000000000FF00ull) << 40) | (((x) & 0x00000000000000FFull) << 56);
}

inline void etcdec__decompress_legacy_etc_mode(unsigned long long block, int r0, int g0, int b0,
                                               int r1, int g1, int b1, unsigned char* decompressed,
                                               int isOpaque, int destinationPitch) {
    int flipBit, codeWord0, codeWord1;
    int i, j, x0, y0, x1, y1, m, idx;
    const int (*modifiersTablePtr)[4];

    /* already remapped so we can just use pixel indices "as-is" */
    static const int modifierTableRemappedOpaque[8][4] = {
        {2, 8, -2, -8},     {5, 17, -5, -17},   {9, 29, -9, -29},     {13, 42, -13, -42},
        {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};

    static const int modifierTableRemappedTransparent[8][4] = {
        {0, 8, 0, -8},   {0, 17, 0, -17}, {0, 29, 0, -29},   {0, 42, 0, -42},
        {0, 60, 0, -60}, {0, 80, 0, -80}, {0, 106, 0, -106}, {0, 183, 0, -183}};

    flipBit = (block & 0x100000000ull) != 0;

    codeWord0 = (block >> 37) & 0x7;
    codeWord1 = (block >> 34) & 0x7;

    modifiersTablePtr = isOpaque ? modifierTableRemappedOpaque : modifierTableRemappedTransparent;

    /* now decode both blocks, using proper orientation based on flipBit */
    for (i = 0; i < 2; ++i) {
        for (j = 0; j < 4; ++j) {
            x0 = flipBit ? i : j;
            x1 = flipBit ? (i + 2) : j;
            y0 = flipBit ? j : i;
            y1 = flipBit ? j : (i + 2);

            /* if isOpaque == 0 and idx is "msb=1 & lsb=0" (== 2) -> pixel is completely transparent
             */

            /* block A */
            m = x0 + y0 * 4;
            idx = (((block >> (m + 16)) & 1) << 1) | ((block >> m) & 1);
            m = (x0 * destinationPitch) + (y0 * 4);
            if (isOpaque || idx != 2) {
                decompressed[m + 0] =
                    (unsigned char)etcdec__clamp_255(r0 + modifiersTablePtr[codeWord0][idx]);
                decompressed[m + 1] =
                    (unsigned char)etcdec__clamp_255(g0 + modifiersTablePtr[codeWord0][idx]);
                decompressed[m + 2] =
                    (unsigned char)etcdec__clamp_255(b0 + modifiersTablePtr[codeWord0][idx]);
                decompressed[m + 3] = 0xFF;
            } else {
                *((unsigned int*)(decompressed + m)) = 0u;
            }

            // block B
            m = x1 + y1 * 4;
            idx = (((block >> (m + 16)) & 1) << 1) | ((block >> m) & 1);
            m = (x1 * destinationPitch) + (y1 * 4);
            if (isOpaque || idx != 2) {
                decompressed[m + 0] =
                    (unsigned char)etcdec__clamp_255(r1 + modifiersTablePtr[codeWord1][idx]);
                decompressed[m + 1] =
                    (unsigned char)etcdec__clamp_255(g1 + modifiersTablePtr[codeWord1][idx]);
                decompressed[m + 2] =
                    (unsigned char)etcdec__clamp_255(b1 + modifiersTablePtr[codeWord1][idx]);
                decompressed[m + 3] = 0xFF;
            } else {
                *((unsigned int*)(decompressed + m)) = 0u;
            }
        }
    }
}

inline void etcdec__decompress_etc_mode_t_h(unsigned long long block, int mode,
                                            unsigned char* decompressed, int isOpaque,
                                            int destinationPitch) {
    int r0, g0, b0, r1, g1, b1;
    int ra, rb, ga, gb, ba, bb, da, db, dist;
    int i, j, k, idx;
    unsigned int paintColors[4]; /* 0xAABBGGRR */

    static const char distanceTable[8] = {3, 6, 11, 16, 23, 32, 41, 64};

    if (mode == 1) { /* "T" mode */
        ra = (block >> 59) & 0x3;
        rb = (block >> 56) & 0x3;
        g0 = (block >> 52) & 0xF;
        b0 = (block >> 48) & 0xF;
        r1 = (block >> 44) & 0xF;
        g1 = (block >> 40) & 0xF;
        b1 = (block >> 36) & 0xF;
        da = (block >> 34) & 0x3;
        db = (block >> 32) & 0x1;

        r0 = (ra << 2) | rb;
    } else { /* "H" mode */
        r0 = (block >> 59) & 0xF;
        ga = (block >> 56) & 0x7;
        gb = (block >> 52) & 0x1;
        ba = (block >> 51) & 0x1;
        bb = (block >> 47) & 0x7;
        r1 = (block >> 43) & 0xF;
        g1 = (block >> 39) & 0xF;
        b1 = (block >> 35) & 0xF;
        da = (block >> 34) & 0x1;
        db = (block >> 32) & 0x1;

        g0 = (ga << 1) | gb;
        b0 = (ba << 3) | bb;
    }

    /* These four bit values are extended to RGB888 by replicating
       the four higher order bits in the four lower order bits. */
    r0 = (r0 << 4) | r0;
    g0 = (g0 << 4) | g0;
    b0 = (b0 << 4) | b0;
    r1 = (r1 << 4) | r1;
    g1 = (g1 << 4) | g1;
    b1 = (b1 << 4) | b1;

    if (mode == 1) { /* "T" mode */
        dist = (da << 1) | db;
        dist = distanceTable[dist];

        paintColors[0] = 0xFF000000 | (b0 << 16) | (g0 << 8) | r0;
        paintColors[2] = 0xFF000000 | (b1 << 16) | (g1 << 8) | r1;
        paintColors[1] = 0xFF000000 | (etcdec__clamp_255(b1 + dist) << 16) |
                         (etcdec__clamp_255(g1 + dist) << 8) | etcdec__clamp_255(r1 + dist);
        paintColors[3] = 0xFF000000 | (etcdec__clamp_255(b1 - dist) << 16) |
                         (etcdec__clamp_255(g1 - dist) << 8) | etcdec__clamp_255(r1 - dist);
    } else { /* "H" mode */
        dist = ((r0 << 16) | (g0 << 8) | b0) >= ((r1 << 16) | (g1 << 8) | b1) ? 1 : 0;
        dist |= (da << 2) | (db << 1);
        dist = distanceTable[dist];

        paintColors[0] = 0xFF000000 | (etcdec__clamp_255(b0 + dist) << 16) |
                         (etcdec__clamp_255(g0 + dist) << 8) | etcdec__clamp_255(r0 + dist);
        paintColors[1] = 0xFF000000 | (etcdec__clamp_255(b0 - dist) << 16) |
                         (etcdec__clamp_255(g0 - dist) << 8) | etcdec__clamp_255(r0 - dist);
        paintColors[2] = 0xFF000000 | (etcdec__clamp_255(b1 + dist) << 16) |
                         (etcdec__clamp_255(g1 + dist) << 8) | etcdec__clamp_255(r1 + dist);
        paintColors[3] = 0xFF000000 | (etcdec__clamp_255(b1 - dist) << 16) |
                         (etcdec__clamp_255(g1 - dist) << 8) | etcdec__clamp_255(r1 - dist);
    }

    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            k = i + j * 4;
            idx = (((block >> (k + 16)) & 1) << 1) | ((block >> k) & 1);
            /* if isOpaque == 0 and idx is "msb=1 & lsb=0" (== 2) -> pixel is completely transparent
             */
            if (isOpaque || idx != 2) {
                ((unsigned int*)decompressed)[j] = paintColors[idx];
            } else {
                ((unsigned int*)decompressed)[j] = 0u;
            }
        }

        decompressed += destinationPitch;
    }
}

inline void etcdec__decompress_etc_mode_planar(unsigned long long block,
                                               unsigned char* decompressed, int destinationPitch) {
    int ro, go, bo, rh, gh, bh, rv, gv, bv;
    int go1, go2, bo1, bo2, bo3, rh1, rh2;
    int i, j;

    ro = (block >> 57) & 0x3F;
    go1 = (block >> 56) & 0x01;
    go2 = (block >> 49) & 0x3F;
    bo1 = (block >> 48) & 0x01;
    bo2 = (block >> 43) & 0x03;
    bo3 = (block >> 39) & 0x07;
    rh1 = (block >> 34) & 0x1F;
    rh2 = (block >> 32) & 0x01;
    gh = (block >> 25) & 0x7F;
    bh = (block >> 19) & 0x3F;
    rv = (block >> 13) & 0x3F;
    gv = (block >> 6) & 0x7F;
    bv = (block >> 0) & 0x3F;

    go = (go1 << 6) | go2;
    bo = (bo1 << 5) | (bo2 << 3) | bo3;
    rh = (rh1 << 1) | rh2;

    ro = (ro << 2) | (ro >> 4);
    rh = (rh << 2) | (rh >> 4);
    rv = (rv << 2) | (rv >> 4);
    go = (go << 1) | (go >> 6);
    gh = (gh << 1) | (gh >> 6);
    gv = (gv << 1) | (gv >> 6);
    bo = (bo << 2) | (bo >> 4);
    bh = (bh << 2) | (bh >> 4);
    bv = (bv << 2) | (bv >> 4);

    /* With three base colors in RGB888 format, the color of each
       pixel can then be determined as:
        R(x, y) = x * (RH − RO) / 4.0 + y * (RV − RO) / 4.0 + RO
        G(x, y) = x * (GH − GO) / 4.0 + y * (GV − GO) / 4.0 + GO
        B(x, y) = x * (BH − BO) / 4.0 + y * (BV − BO) / 4.0 + BO */
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            decompressed[(j * 4) + 0] = (unsigned char)etcdec__clamp_255(
                (j * (rh - ro) + i * (rv - ro) + (ro << 2) + 2) >> 2);
            decompressed[(j * 4) + 1] = (unsigned char)etcdec__clamp_255(
                (j * (gh - go) + i * (gv - go) + (go << 2) + 2) >> 2);
            decompressed[(j * 4) + 2] = (unsigned char)etcdec__clamp_255(
                (j * (bh - bo) + i * (bv - bo) + (bo << 2) + 2) >> 2);
            decompressed[(j * 4) + 3] = 0xFF;
        }

        decompressed += destinationPitch;
    }
}

inline void etcdec__decompress_etc_block(const void* compressedBlock, void* decompressedBlock,
                                         int isPunchthrough, int destinationPitch) {
    unsigned long long block;
    int diffBit, newMode;
    int r0, g0, b0, r1, g1, b1;
    unsigned char* decompressed;

    std::memcpy(&block, compressedBlock, sizeof(block));
    block = etcdec__bswap(block);
    decompressed = (unsigned char*)decompressedBlock;

    /* if isPunchthrough == TRUE -> this is actually an 'opaque' bit */
    diffBit = (block & 0x200000000ull) != 0;

    newMode = 0; /* assume legacy mode by default */

    if (!isPunchthrough && !diffBit) { /* "individual" mode */
        r0 = (block >> 60) & 0xF;
        r1 = (block >> 56) & 0xF;
        g0 = (block >> 52) & 0xF;
        g1 = (block >> 48) & 0xF;
        b0 = (block >> 44) & 0xF;
        b1 = (block >> 40) & 0xF;

        /* These four bit values are extended to RGB888 by replicating
           the four higher order bits in the four lower order bits. */
        r0 = (r0 << 4) | r0;
        g0 = (g0 << 4) | g0;
        b0 = (b0 << 4) | b0;
        r1 = (r1 << 4) | r1;
        g1 = (g1 << 4) | g1;
        b1 = (b1 << 4) | b1;
    } else { /* "differential" mode */
        r0 = (block >> 59) & 0x1F;
        r1 = r0 + etcdec__extend_sign((block >> 56) & 0x7, 3);
        g0 = (block >> 51) & 0x1F;
        g1 = g0 + etcdec__extend_sign((block >> 48) & 0x7, 3);
        b0 = (block >> 43) & 0x1F;
        b1 = b0 + etcdec__extend_sign((block >> 40) & 0x7, 3);

        if (r1 < 0 || r1 > 31) {
            /* First, R and dR are added, and if the sum is not
               within the interval[0, 31], the "T" mode is selected */
            newMode = 1;
        } else if (g1 < 0 || g1 > 31) {
            /* Otherwise, if the sum of Gand dG is outside
               the interval[0, 31], the "H" mode is selected */
            newMode = 2;
        } else if (b1 < 0 || b1 > 31) {
            /* Otherwise, if the sum of Band dB is outside
               of the interval[0, 31], the "planar" mode is selected */
            newMode = 3;
        } else {
            /* Finally the "differential" mode is selected */

            /* These five-bit codewords are extended to RGB888 by replicating
               the top three highest order bits to the three lowest order bits. */
            r0 = (r0 << 3) | (r0 >> 2);
            g0 = (g0 << 3) | (g0 >> 2);
            b0 = (b0 << 3) | (b0 >> 2);
            r1 = (r1 << 3) | (r1 >> 2);
            g1 = (g1 << 3) | (g1 >> 2);
            b1 = (b1 << 3) | (b1 >> 2);
        }
    }

    if (!newMode) { /* legacy mode: ETC1 & ETC2 */
        etcdec__decompress_legacy_etc_mode(block, r0, g0, b0, r1, g1, b1, decompressed,
                                           !isPunchthrough || diffBit, destinationPitch);
    } else if (newMode < 3) { /* ETC2 "T" and "H" modes */
        etcdec__decompress_etc_mode_t_h(block, newMode, decompressed, !isPunchthrough || diffBit,
                                        destinationPitch);
    } else { /* ETC2 "planar" mode */
        etcdec__decompress_etc_mode_planar(block, decompressed, destinationPitch);
    }
}

inline constexpr int EacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};
inline int EacValue(const std::uint8_t* block, unsigned x, unsigned y, bool signed_value,
                    bool alpha = false) {
    std::uint64_t bits{};
    for (unsigned i = 2; i < 8; ++i)
        bits = (bits << 8) | block[i];
    const auto index = (bits >> (45 - 3 * (x * 4 + y))) & 7;
    const int multiplier = block[1] >> 4;
    const int modifier = EacModifiers[block[1] & 15][index];
    if (alpha)
        return std::clamp(int(block[0]) + modifier * multiplier, 0, 255);
    const int base =
        signed_value ? std::max(-127, int(static_cast<std::int8_t>(block[0]))) : int(block[0]);
    const int value =
        base * 8 + (signed_value ? 0 : 4) + modifier * (multiplier ? multiplier * 8 : 1);
    return std::clamp(value, signed_value ? -1023 : 0, signed_value ? 1023 : 2047);
}
} // namespace FormatDetail

// CPU conversion is used for packed formats so render targets and UAVs have
// canonical RGBA channels too; an SRV-only swizzle cannot fix writes.
inline std::array<unsigned, 4> PackedWidths(std::string_view name) {
    if (name == "B5G6R5_UNORM")
        return {5, 6, 5, 0};
    if (name == "G4R4_UNORM")
        return {4, 4, 0, 0};
    if (name == "A4B4G4R4_UNORM")
        return {4, 4, 4, 4};
    if (name == "A2R10G10B10_UNORM")
        return {10, 10, 10, 2};
    if (name == "A5B5G5R1_UNORM")
        return {1, 5, 5, 5};
    return {5, 5, 5, 1};
}
inline unsigned PackedBytes(std::string_view name) {
    return name == "G4R4_UNORM" ? 1u : name == "A2R10G10B10_UNORM" ? 4u : 2u;
}
inline void ConvertPackedRow(std::string_view name, std::uint8_t* host, std::uint8_t* guest,
                             unsigned width, bool download) {
    const auto widths = PackedWidths(name);
    const auto bytes = PackedBytes(name);
    const bool wide = name == "A2R10G10B10_UNORM";
    const unsigned host_bytes = wide ? 8 : 4;
    const unsigned host_max = wide ? 65535 : 255;
    const bool reverse = name == "A2R10G10B10_UNORM";
    for (unsigned x = 0; x < width; ++x) {
        std::uint32_t packed{};
        if (!download)
            std::memcpy(&packed, guest + x * bytes, bytes);
        unsigned shift{};
        for (unsigned c = 0; c < 4; ++c) {
            const auto channel = reverse && c < 3 ? 2 - c : c;
            const auto mask = (1u << widths[c]) - 1;
            if (download) {
                unsigned value{};
                if (wide) {
                    std::uint16_t sample{};
                    std::memcpy(&sample, host + x * host_bytes + channel * 2, 2);
                    value = sample;
                } else
                    value = host[x * host_bytes + channel];
                if (mask)
                    packed |= ((value * mask + host_max / 2) / host_max) << shift;
            } else {
                const auto value = mask ? (((packed >> shift) & mask) * host_max + mask / 2) / mask
                                   : c == 3 ? host_max
                                            : 0;
                if (wide) {
                    const auto sample = static_cast<std::uint16_t>(value);
                    std::memcpy(host + x * host_bytes + channel * 2, &sample, 2);
                } else
                    host[x * host_bytes + channel] = static_cast<std::uint8_t>(value);
            }
            shift += widths[c];
        }
        if (download)
            std::memcpy(guest + x * bytes, &packed, bytes);
    }
}
inline void DecodeEtcBlock(FormatInfo format, const std::uint8_t* input, std::uint8_t* output) {
    if (format.conversion == FormatConversion::Etc2) {
        const bool rgba = format.name.find("RGBA") != std::string_view::npos;
        const bool punchthrough = format.name.find("PTA") != std::string_view::npos;
        FormatDetail::etcdec__decompress_etc_block(input + (rgba ? 8 : 0), output, punchthrough,
                                                   16);
        if (rgba)
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x)
                    output[(y * 4 + x) * 4 + 3] =
                        static_cast<std::uint8_t>(FormatDetail::EacValue(input, x, y, false, true));
        return;
    }
    const unsigned channels = format.bytes_per_block / 2;
    const bool signed_value = format.name.ends_with("SNORM");
    for (unsigned c = 0; c < channels; ++c)
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x) {
                const int value = FormatDetail::EacValue(input + c * 8, x, y, signed_value);
                const int scaled = signed_value ? value * 32767 / 1023 : value * 65535 / 2047;
                const auto bits = static_cast<std::uint16_t>(scaled);
                std::memcpy(output + ((y * 4 + x) * channels + c) * 2, &bits, 2);
            }
}
// D3D12 stores depth/stencil in separate copy planes. Plane zero is 32-bit
// depth (24 meaningful bits for D24), plane one is an 8-bit stencil value.
inline void ConvertDepthRow(std::string_view name, std::uint8_t* host, std::uint8_t* guest,
                            unsigned width, unsigned plane, bool download) {
    const bool float_depth = name == "D32_FLOAT_S8_UINT";
    const bool stencil_only = name == "S8_UINT";
    const bool stencil_first = name == "S8_UINT_D24_UNORM";
    const unsigned guest_bytes = stencil_only ? 1 : float_depth ? 8 : 4;
    for (unsigned x = 0; x < width; ++x) {
        auto* pixel = guest + x * guest_bytes;
        if (plane == 1) {
            const unsigned position = stencil_only || stencil_first ? 0 : float_depth ? 4 : 3;
            if (download) {
                if (name != "X8_D24_UNORM")
                    pixel[position] = host[x];
            } else
                host[x] = name == "X8_D24_UNORM" ? 0 : pixel[position];
        } else {
            std::uint32_t value{};
            if (download) {
                std::memcpy(&value, host + x * 4, 4);
                if (float_depth)
                    std::memcpy(pixel, &value, 4);
                else if (!stencil_only) {
                    const unsigned start = stencil_first ? 1 : 0;
                    std::memcpy(pixel + start, &value, 3);
                }
            } else {
                if (float_depth)
                    std::memcpy(&value, pixel, 4);
                else if (!stencil_only)
                    std::memcpy(&value, pixel + (stencil_first ? 1 : 0), 3);
                std::memcpy(host + x * 4, &value, 4);
            }
        }
    }
}
} // namespace D3D12
