# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise MSAA pipeline specialization and throwing slot construction on the host."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <stdexcept>
#include <string>
#include "common/slot_vector.h"
#include "common/deferred_exception.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
struct Image {
    static inline int alive = 0;
    int value;
    explicit Image(int v) : value(v) {
        if (v < 0) throw std::runtime_error("D3D12 unsupported test format");
        ++alive;
    }
    Image(Image&& other) noexcept : value(other.value) { ++alive; }
    ~Image() { --alive; }
};
int main() {
    using namespace D3D12::Pso;
    GraphicsPsoState single{}, multi{};
    FramebufferFormats formats{};
    formats.colors[0] = 28;
    formats.depth = 45;
    ApplyFramebufferFormats(single, formats);
    assert(single.sample_count == 1);
    for (unsigned samples : {2, 4, 8, 16}) {
        formats.sample_count = samples;
        ApplyFramebufferFormats(multi, formats);
        assert(multi.sample_count == samples);
        assert(HashBytes(&single, sizeof(single)) != HashBytes(&multi, sizeof(multi)));
    }
    formats.colors = {};
    ApplyFramebufferFormats(multi, formats);
    assert(multi.num_render_targets == 0 && multi.dsv_format == 45 && multi.sample_count == 16);
    {
        Common::SlotVector<Image> images;
        const auto first = images.insert(17);
        Common::DeferredException failure;
        // Match the macro JIT boundary, including failure from SlotVector::insert.
        failure.Invoke([&] { (void)images.insert(-1); });
        assert(failure.HasError() && images.size() == 1 && images[first].value == 17);
        bool reported = false;
        try { failure.Rethrow(); }
        catch (const std::exception& error) {
            reported = std::string(error.what()) == "D3D12 unsupported test format";
        }
        assert(reported && !failure.HasError());
        const auto second = images.insert(23);
        assert(images.size() == 2 && images[second].value == 23);
        images.erase(second);
        // Repeated failures must not consume slots or destroy the existing image.
        for (int i = 0; i < 100; ++i) {
            try { (void)images.insert(-1); assert(false); }
            catch (const std::runtime_error&) {}
            assert(images.size() == 1 && Image::alive == 1);
        }
        const auto reused = images.insert(31);
        assert(reused == second && images[first].value == 17);
    }
    assert(Image::alive == 0);
}
"""


RESOURCE_DRIVER = r"""
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include "video_core/renderer_d3d12/d3d12_formats.h"
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using UINT16 = std::uint16_t;
using UINT = u32;
using DXGI_FORMAT = u32;
using HRESULT = std::int32_t;
constexpr bool FAILED(HRESULT value) { return value < 0; }
constexpr bool SUCCEEDED(HRESULT value) { return value >= 0; }
namespace VideoCore::Surface {
enum class PixelFormat { A8B8G8R8_UNORM, D24_UNORM_S8_UINT, Invalid };
}
#define PIXEL_FORMAT_LIST PIXEL_FORMAT_ELEM(A8B8G8R8_UNORM) PIXEL_FORMAT_ELEM(D24_UNORM_S8_UINT)
namespace VideoCommon {
enum class ImageType : u32 { e1D, e2D, e3D, Buffer, Linear };
struct ImageInfo {
    VideoCore::Surface::PixelFormat format{VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM};
    ImageType type{ImageType::e2D};
    struct { u32 width=3840, height=2160, depth=1; } size;
    struct { int levels=1, layers=1; } resources;
    u32 num_samples=4;
};
}
#include "video_core/texture_cache/samples_helper.h"
namespace fmt {
template <typename... Args> std::string format(std::string_view text, const Args&...) {
    return std::string{text};
}
}
static unsigned warnings=0;
static std::string last_condition;
template <typename... Args> void RecordLog(std::string_view, Args&&... args) {
    ++warnings;
    last_condition=std::get<sizeof...(Args)-1>(std::forward_as_tuple(args...));
}
#define LOG_WARNING(category, ...) RecordLog(__VA_ARGS__)
enum {
    D3D12_RESOURCE_DIMENSION_TEXTURE1D=1, D3D12_RESOURCE_DIMENSION_TEXTURE2D=2,
    D3D12_RESOURCE_DIMENSION_TEXTURE3D=3, D3D12_TEXTURE_LAYOUT_UNKNOWN=0,
    D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE=0, D3D12_FEATURE_FORMAT_SUPPORT=1,
    D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS=2, D3D12_HEAP_TYPE_DEFAULT=0,
    D3D12_RESOURCE_STATE_COMMON=0, D3D12_FORMAT_SUPPORT1_TEXTURE1D=1,
    D3D12_FORMAT_SUPPORT1_TEXTURE2D=2, D3D12_FORMAT_SUPPORT1_TEXTURE3D=4,
    D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL=8, D3D12_FORMAT_SUPPORT1_RENDER_TARGET=16,
    D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE=1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL=1,
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET=2, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS=4,
    DXGI_FORMAT_R8G8B8A8_UNORM=28,
};
inline bool BisectEnabled(const char*) { return false; }
struct D3D12_RESOURCE_DESC {
    int Dimension{};
    u64 Width{};
    u32 Height{};
    UINT16 DepthOrArraySize{}, MipLevels{};
    DXGI_FORMAT Format{};
    struct { u32 Count{}, Quality{}; } SampleDesc;
    int Layout{};
    u32 Flags{};
};
struct D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS {
    DXGI_FORMAT Format{};
    u32 SampleCount{}, Flags{}, NumQualityLevels{};
};
struct D3D12_FEATURE_DATA_FORMAT_SUPPORT {
    DXGI_FORMAT Format{};
    u32 Support1{}, Support2{};
};
namespace D3D12 {
struct Resource { D3D12_RESOURCE_DESC desc; };
using ResourcePtr=std::shared_ptr<Resource>;
struct TestDevice {
    u32 qualities=1, requested_samples=0;
    DXGI_FORMAT requested_format=0;
    HRESULT CheckFeatureSupport(int feature,void* data,std::size_t) {
        if (feature==D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS) {
            auto& quality=*static_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS*>(data);
            requested_samples=quality.SampleCount;
            requested_format=quality.Format;
            quality.NumQualityLevels=qualities;
        } else {
            auto& support=*static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
            support.Support1=1|2|4|8|16;
            support.Support2=1;
        }
        return 0;
    }
    HRESULT GetDeviceRemovedReason() { return 0; }
};
struct Context {
    TestDevice device;
    unsigned allocations=0;
    TestDevice* Device() { return &device; }
    ResourcePtr CreateResource(D3D12_RESOURCE_DESC desc,int,int) {
        ++allocations;
        return std::make_shared<Resource>(Resource{desc});
    }
};
void Check(HRESULT result,const char*,TestDevice*) {
    if (FAILED(result)) throw std::runtime_error("device removed");
}
DXGI_FORMAT DsvFormat(DXGI_FORMAT format) { return format==46 ? 45 : format; }
__CREATION__
} // namespace D3D12
int main() {
    using namespace D3D12;
    Context context;
    VideoCommon::ImageInfo info;
    auto format=LookupFormat("A8B8G8R8_UNORM");
    auto image=MakeTexture(context,info,format);
    assert(image && context.device.requested_format==28 && context.device.requested_samples==4);
    assert(image->desc.SampleDesc.Count==4 && image->desc.SampleDesc.Quality==0);
    assert(image->desc.Width==1920 && image->desc.Height==1080 && image->desc.MipLevels==1);
    assert(image->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    assert(!(image->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    info.format=VideoCore::Surface::PixelFormat::D24_UNORM_S8_UINT;
    format=LookupFormat("D24_UNORM_S8_UINT");
    image=MakeTexture(context,info,format);
    assert(image && image->desc.Format==44 && context.device.requested_format==45);
    assert(image->desc.Flags==D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    assert(image->desc.Width==1920 && image->desc.SampleDesc.Count==4);
    context.device.qualities=0;
    const auto allocations=context.allocations;
    const auto before=warnings;
    assert(!MakeTexture(context,info,format));
    assert(last_condition=="MULTISAMPLE_QUALITY_LEVELS.NumQualityLevels == 0");
    for (int i=0;i<20;++i) assert(!MakeTexture(context,info,format));
    assert(warnings==before+1 && context.allocations==allocations);
    context.device.qualities=1;
    info.num_samples=3;
    assert(!MakeTexture(context,info,format));
    info.num_samples=4;
    info.resources.levels=2;
    assert(!MakeTexture(context,info,format));
    info.resources.levels=1;
    info.type=VideoCommon::ImageType::e3D;
    assert(!MakeTexture(context,info,format));
    info.type=VideoCommon::ImageType::e2D;
    info.num_samples=1;
    info.format=VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    format=LookupFormat("A8B8G8R8_UNORM");
    image=MakeTexture(context,info,format);
    assert(image && image->desc.SampleDesc.Count==1 && image->desc.Width==3840);
    assert(image->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    info.size.width=20000;
    assert(!MakeTexture(context,info,format));
    info.size.width=1;
    info.resources.levels=16;
    assert(!MakeTexture(context,info,format));
    info.resources.levels=1;
    format.planes=3;
    assert(!MakeTexture(context,info,format));
    format=LookupFormat("");
    assert(!MakeTexture(context,info,format));
}
"""


class D3D12MsaaTests(unittest.TestCase):
    def test_pipeline_samples_and_renderer_constructor_exception(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-msaa-") as directory:
            folder = Path(directory)
            common = folder / "common"
            common.mkdir()
            # The container itself is real; only the logging/assert dependency is isolated.
            (common / "assert.h").write_text(
                "#pragma once\n#include <cassert>\n#define DEBUG_ASSERT(x) assert(x)\n"
            )
            source = folder / "msaa.cpp"
            binary = folder / "msaa"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(folder),
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_real_image_creation_policy_without_gpu_sdk(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        creation = source.split("namespace D3D12 {", 1)[1].split("UINT Subresource", 1)[0]
        # Close the anonymous namespace containing MakeTexture.
        creation += "\n}\n"
        with tempfile.TemporaryDirectory(prefix="nxbox-msaa-resource-") as directory:
            folder = Path(directory)
            common = folder / "common"
            common.mkdir()
            (common / "assert.h").write_text(
                "#pragma once\n#include <cassert>\n"
                "#define ASSERT(x) assert(x)\n#define ASSERT_MSG(x, ...) assert(x)\n"
            )
            cpp = folder / "creation.cpp"
            binary = Path(directory) / "creation"
            cpp.write_text(RESOURCE_DRIVER.replace("__CREATION__", creation))
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(folder),
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
