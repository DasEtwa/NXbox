// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace D3D12 {
// DXGI numeric values keep this table testable without a Windows SDK. Unknown (0)
// means the format needs a conversion that P2 does not implement.
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
    constexpr bool Supported() const {
        return resource_format != 0;
    }
};
inline constexpr auto FormatTable = std::to_array<FormatInfo>({
    {"A8B8G8R8_UNORM", 27, 28, 1, 1, 4, false, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"A8B8G8R8_SNORM", 27, 31, 1, 1, 4, false, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_SNORM
    {"A8B8G8R8_SINT", 27, 32, 1, 1, 4, false, false, 1},  // R8G8B8A8_TYPELESS, R8G8B8A8_SINT
    {"A8B8G8R8_UINT", 27, 30, 1, 1, 4, false, false, 1},  // R8G8B8A8_TYPELESS, R8G8B8A8_UINT
    {"R5G6B5_UNORM", 85, 85, 1, 1, 2, false, false, 1},   // B5G6R5_UNORM, B5G6R5_UNORM
    {"B5G6R5_UNORM", 0, 0, 1, 1, 2, false, false, 1},     // UNKNOWN, UNKNOWN
    {"A1R5G5B5_UNORM", 86, 86, 1, 1, 2, false, false, 1}, // B5G5R5A1_UNORM, B5G5R5A1_UNORM
    {"A2B10G10R10_UNORM", 23, 24, 1, 1, 4, false, false,
     1}, // R10G10B10A2_TYPELESS, R10G10B10A2_UNORM
    {"A2B10G10R10_UINT", 23, 25, 1, 1, 4, false, false,
     1},                                                   // R10G10B10A2_TYPELESS, R10G10B10A2_UINT
    {"A2R10G10B10_UNORM", 0, 0, 1, 1, 4, false, false, 1}, // UNKNOWN, UNKNOWN
    {"A1B5G5R5_UNORM", 0, 0, 1, 1, 2, false, false, 1},    // UNKNOWN, UNKNOWN
    {"A5B5G5R1_UNORM", 0, 0, 1, 1, 2, false, false, 1},    // UNKNOWN, UNKNOWN
    {"R8_UNORM", 60, 61, 1, 1, 1, false, false, 1},        // R8_TYPELESS, R8_UNORM
    {"R8_SNORM", 60, 63, 1, 1, 1, false, false, 1},        // R8_TYPELESS, R8_SNORM
    {"R8_SINT", 60, 64, 1, 1, 1, false, false, 1},         // R8_TYPELESS, R8_SINT
    {"R8_UINT", 60, 62, 1, 1, 1, false, false, 1},         // R8_TYPELESS, R8_UINT
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
    {"R16G16B16X16_FLOAT", 9, 10, 1, 1, 8, false, false,
     1},                                            // R16G16B16A16_TYPELESS, R16G16B16A16_FLOAT
    {"R32_UINT", 39, 42, 1, 1, 4, false, false, 1}, // R32_TYPELESS, R32_UINT
    {"R32_SINT", 39, 43, 1, 1, 4, false, false, 1}, // R32_TYPELESS, R32_SINT
    {"ASTC_2D_8X8_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_8X5_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"ASTC_2D_5X4_UNORM", 27, 28, 1, 1, 4, true, false, 1}, // R8G8B8A8_TYPELESS, R8G8B8A8_UNORM
    {"B8G8R8A8_SRGB", 90, 91, 1, 1, 4, false, false, 1},   // B8G8R8A8_TYPELESS, B8G8R8A8_UNORM_SRGB
    {"BC1_RGBA_SRGB", 70, 72, 4, 4, 8, false, false, 1},   // BC1_TYPELESS, BC1_UNORM_SRGB
    {"BC2_SRGB", 73, 75, 4, 4, 16, false, false, 1},       // BC2_TYPELESS, BC2_UNORM_SRGB
    {"BC3_SRGB", 76, 78, 4, 4, 16, false, false, 1},       // BC3_TYPELESS, BC3_UNORM_SRGB
    {"BC7_SRGB", 97, 99, 4, 4, 16, false, false, 1},       // BC7_TYPELESS, BC7_UNORM_SRGB
    {"A4B4G4R4_UNORM", 0, 0, 1, 1, 2, false, false, 1},    // UNKNOWN, UNKNOWN
    {"G4R4_UNORM", 0, 0, 1, 1, 1, false, false, 1},        // UNKNOWN, UNKNOWN
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
    {"ETC2_RGB_UNORM", 0, 0, 4, 4, 8, false, false, 1},    // UNKNOWN, UNKNOWN
    {"ETC2_RGBA_UNORM", 0, 0, 4, 4, 16, false, false, 1},  // UNKNOWN, UNKNOWN
    {"ETC2_RGB_PTA_UNORM", 0, 0, 4, 4, 8, false, false, 1}, // UNKNOWN, UNKNOWN
    {"ETC2_RGB_SRGB", 0, 0, 4, 4, 8, false, false, 1},      // UNKNOWN, UNKNOWN
    {"ETC2_RGBA_SRGB", 0, 0, 4, 4, 16, false, false, 1},    // UNKNOWN, UNKNOWN
    {"ETC2_RGB_PTA_SRGB", 0, 0, 4, 4, 8, false, false, 1},  // UNKNOWN, UNKNOWN
    {"EAC_R11_UNORM", 0, 0, 4, 4, 8, false, false, 1},      // UNKNOWN, UNKNOWN
    {"EAC_R11_SNORM", 0, 0, 4, 4, 8, false, false, 1},      // UNKNOWN, UNKNOWN
    {"EAC_R11G11_UNORM", 0, 0, 4, 4, 16, false, false, 1},  // UNKNOWN, UNKNOWN
    {"EAC_R11G11_SNORM", 0, 0, 4, 4, 16, false, false, 1},  // UNKNOWN, UNKNOWN
    {"D32_FLOAT", 39, 41, 1, 1, 4, false, true, 1},         // R32_TYPELESS, R32_FLOAT
    {"D16_UNORM", 53, 56, 1, 1, 2, false, true, 1},         // R16_TYPELESS, R16_UNORM
    {"X8_D24_UNORM", 44, 46, 1, 1, 4, false, true, 2},      // R24G8_TYPELESS, R24_UNORM_X8_TYPELESS
    {"S8_UINT", 0, 0, 1, 1, 1, false, true, 1},             // UNKNOWN, UNKNOWN
    {"D24_UNORM_S8_UINT", 44, 46, 1, 1, 4, false, true, 2}, // R24G8_TYPELESS, R24_UNORM_X8_TYPELESS
    {"S8_UINT_D24_UNORM", 0, 0, 1, 1, 4, false, true, 1},   // UNKNOWN, UNKNOWN
    {"D32_FLOAT_S8_UINT", 19, 21, 1, 1, 8, false, true,
     2}, // R32G8X24_TYPELESS, R32_FLOAT_X8X24_TYPELESS
});
constexpr FormatInfo LookupFormat(std::string_view name) {
    for (const auto& format : FormatTable) {
        if (format.name == name)
            return format;
    }
    return {name, 0, 0, 0, 0, 0, false, false, 0};
}
} // namespace D3D12
