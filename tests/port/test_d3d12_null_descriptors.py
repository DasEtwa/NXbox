# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate production null descriptors, including the actual SRV union layout."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cfloat>
#include <cstddef>
using UINT = unsigned;
using u32 = unsigned;
enum D3D12_SRV_DIMENSION {
    D3D12_SRV_DIMENSION_UNKNOWN,
    D3D12_SRV_DIMENSION_BUFFER,
    D3D12_SRV_DIMENSION_TEXTURE1D,
    D3D12_SRV_DIMENSION_TEXTURE1DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2D,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DMS,
    D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY,
    D3D12_SRV_DIMENSION_TEXTURE3D,
    D3D12_SRV_DIMENSION_TEXTURECUBE,
    D3D12_SRV_DIMENSION_TEXTURECUBEARRAY
};
constexpr UINT DXGI_FORMAT_R8G8B8A8_UNORM = 28;
constexpr UINT D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING = 5768;
constexpr UINT D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 0;
constexpr UINT D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER = 1;
constexpr UINT D3D12_FILTER_MIN_MAG_MIP_POINT = 0;
constexpr UINT D3D12_TEXTURE_ADDRESS_MODE_CLAMP = 3;
constexpr UINT D3D12_COMPARISON_FUNC_NEVER = 1;
constexpr float D3D12_FLOAT32_MAX = FLT_MAX;
struct D3D12_SHADER_RESOURCE_VIEW_DESC {
    UINT Format;
    D3D12_SRV_DIMENSION ViewDimension;
    UINT Shader4ComponentMapping;
    union {
        struct {
            unsigned long long FirstElement;
            UINT NumElements, StructureByteStride, Flags;
        } Buffer;
        struct {
            UINT MostDetailedMip, MipLevels;
            float ResourceMinLODClamp;
        } Texture1D, Texture3D, TextureCube;
        struct {
            UINT MostDetailedMip, MipLevels, FirstArraySlice, ArraySize;
            float ResourceMinLODClamp;
        } Texture1DArray;
        struct {
            UINT MostDetailedMip, MipLevels, PlaneSlice;
            float ResourceMinLODClamp;
        } Texture2D;
        struct {
            UINT MostDetailedMip, MipLevels, FirstArraySlice, ArraySize, PlaneSlice;
            float ResourceMinLODClamp;
        } Texture2DArray;
        struct {
            UINT FirstArraySlice, ArraySize;
        } Texture2DMSArray;
        struct {
            UINT MostDetailedMip, MipLevels, First2DArrayFace, NumCubes;
            float ResourceMinLODClamp;
        } TextureCubeArray;
    };
};
struct D3D12_SAMPLER_DESC {
    UINT Filter, AddressU, AddressV, AddressW;
    float MipLODBias;
    UINT MaxAnisotropy, ComparisonFunc;
    float BorderColor[4], MinLOD, MaxLOD;
};
struct DescriptorTable {};
struct D3D12DescriptorAllocator {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D12_SAMPLER_DESC sampler{};
    void WriteCached(UINT type, std::nullptr_t, UINT kind, const void* desc, size_t,
                     const DescriptorTable&, u32 slot) {
        assert(slot == 2);
        if (kind == 1) {
            assert(type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            srv = *static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc);
        } else {
            assert(kind == 3 && type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            sampler = *static_cast<const D3D12_SAMPLER_DESC*>(desc);
        }
    }
    void WriteNullSrv(const DescriptorTable&, u32, D3D12_SRV_DIMENSION);
    void WriteNullSampler(const DescriptorTable&, u32);
};
__PRODUCTION__
int main() {
    D3D12DescriptorAllocator allocator;
    for (unsigned value = 1; value <= 10; ++value) {
        auto dimension = static_cast<D3D12_SRV_DIMENSION>(value);
        allocator.WriteNullSrv({}, 2, dimension);
        const auto& d = allocator.srv;
        assert(d.ViewDimension == dimension);
        assert(d.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
        assert(d.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        switch (dimension) {
        case D3D12_SRV_DIMENSION_BUFFER:
            assert(d.Buffer.NumElements == 1 && d.Buffer.FirstElement == 0);
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            assert(d.Texture2DMSArray.ArraySize == 1 && d.Texture2DMSArray.FirstArraySlice == 0);
            break;
        default:
            // All mip-bearing SRVs share the first two union fields.
            assert(d.TextureCube.MostDetailedMip == 0 && d.TextureCube.MipLevels == 1);
            break;
        }
        if (dimension == D3D12_SRV_DIMENSION_TEXTURECUBE)
            assert(d.TextureCube.ResourceMinLODClamp == 0);
        if (dimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) {
            assert(d.TextureCubeArray.First2DArrayFace == 0 && d.TextureCubeArray.NumCubes == 1);
            assert(d.TextureCubeArray.ResourceMinLODClamp == 0);
        }
        if (dimension == D3D12_SRV_DIMENSION_TEXTURE3D)
            assert(d.Texture3D.ResourceMinLODClamp == 0);
    }
    allocator.WriteNullSampler({}, 2);
    const auto& d = allocator.sampler;
    assert(d.Filter == D3D12_FILTER_MIN_MAG_MIP_POINT);
    assert(d.AddressU == 3 && d.AddressV == 3 && d.AddressW == 3);
    assert(d.MaxAnisotropy >= 1 && d.MaxAnisotropy <= 16);
    assert(d.ComparisonFunc == D3D12_COMPARISON_FUNC_NEVER);
    assert(d.MipLODBias == 0 && d.MinLOD == 0 && d.MaxLOD == FLT_MAX);
}
"""


class NullDescriptors(unittest.TestCase):
    def test_production_null_descriptions(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.cpp").read_text()
        functions = []
        for name in ("WriteNullSrv", "WriteNullSampler"):
            body = source[source.index(f"void D3D12DescriptorAllocator::{name}(") :]
            functions.append(body[: body.index("\n}\n") + 3])
        with tempfile.TemporaryDirectory(prefix="nxbox-null-descriptors-") as directory:
            cpp = Path(directory) / "null.cpp"
            binary = Path(directory) / "null"
            cpp.write_text(DRIVER.replace("__PRODUCTION__", "\n".join(functions)))
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
