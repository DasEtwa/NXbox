# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Execute production viewport/scissor/root-constant code without a Windows SDK."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class WindowOriginTests(unittest.TestCase):
    def test_window_origin_semantics(self):
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        graphics = (NATIVE / "d3d12_graphics_pipeline.h").read_text()
        interface = (NATIVE / "d3d12_pipeline_iface.h").read_text()
        viewport = rasterizer.split("struct ViewportResult {", 1)[1].split(
            "std::optional<VideoCore::Surface::PixelFormat>", 1
        )[0]
        inputs = interface.split("    struct DrawInputs {", 1)[1].split(
            "    virtual std::array<u32, 16> RootConstants", 1
        )[0]
        root_method = graphics.split(
            "    std::array<u32, 16> RootConstants(const DrawInputs& inputs) const override {", 1
        )[1].split("    void SetTransformFeedback", 1)[0]
        root_call = rasterizer.split("    result.words = pipeline.RootConstants(", 1)[1].split(
            ";", 1
        )[0]
        driver = (
            r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "shader_recompiler/backend/hlsl/hlsl_root_constants.h"
using u32 = std::uint32_t;
using s32 = std::int32_t;
using f32 = float;
struct D3D12_VIEWPORT { float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; };
struct D3D12_RECT { s32 left, top, right, bottom; };
namespace ClearOperations {
using Rect = D3D12_RECT;
Rect Intersect(Rect a, Rect b) {
    return {std::max(a.left,b.left),std::max(a.top,b.top),
            std::min(a.right,b.right),std::min(a.bottom,b.bottom)};
}
}
struct Maxwell {
    struct WindowOrigin { enum class Mode { UpperLeft, LowerLeft }; Mode mode; } window_origin;
    enum class ViewportSwizzle { PositiveY, NegativeY };
    enum class DepthMode { ZeroToOne, MinusOneToOne } depth_mode{};
    struct Transform {
        float translate_x=640, scale_x=640, translate_y=360, scale_y=360;
        float translate_z=.5f, scale_z=.5f;
        struct { ViewportSwizzle y{}; } swizzle;
    };
    std::array<Transform, 1> viewport_transform;
    struct { u32 x=0, y=0, width=1280, height=720; } surface_clip;
    struct Scissor { bool enable=true; s32 min_x=300, min_y=200, max_x=500, max_y=400; };
    std::array<Scissor, 1> scissor_test;
    bool viewport_scale_offset_enabled=true;
};
struct ViewportResult {"""
            + viewport
            + r"""
struct Pipeline {
    struct DrawInputs {"""
            + inputs
            + r"""
    std::array<u32, 16> RootConstants(const DrawInputs& inputs) const {"""
            + root_method
            + r"""
};
std::array<u32, 16> ComputeWords(const Maxwell& regs) {
    const u32 width=1280, height=720;
    [[maybe_unused]] const bool y_flip = (regs.viewport_scale_offset_enabled
        ? MakeViewport(regs, 0) : MakeFullViewport(regs)).y_flip;
    Pipeline pipeline;
    return pipeline.RootConstants("""
            + root_call
            + r""";
}
int main() {
    int failures=0;
    for (bool lower_left : {false,true}) for (bool scaled : {false,true})
      for (float scale_y : {-360.f,360.f}) for (bool swizzle : {false,true}) {
        Maxwell regs{};
        regs.window_origin.mode = lower_left ? Maxwell::WindowOrigin::Mode::LowerLeft
                                            : Maxwell::WindowOrigin::Mode::UpperLeft;
        regs.viewport_scale_offset_enabled = scaled;
        regs.viewport_transform[0].scale_y = scale_y;
        regs.viewport_transform[0].swizzle.y = swizzle ? Maxwell::ViewportSwizzle::NegativeY
                                                      : Maxwell::ViewportSwizzle::PositiveY;
        auto words=ComputeWords(regs);
        float direction;
        std::memcpy(&direction, &words[5], sizeof(direction));
        // SPIR-V EmitYDirection: only window origin controls the guest S2R value.
        if (direction != (lower_left ? -1.f : 1.f)) {
            std::cerr << "YDirection mismatch: lower_left=" << lower_left
                      << " scaled=" << scaled << " scale_y=" << scale_y
                      << " swizzle=" << swizzle << '\n';
            ++failures;
        }
        if (!scaled) {
            // Vulkan's untransformed path uses surface_clip, ignoring stale scissor registers.
            const auto clip=MakeScissor(regs,0);
            if (clip.left!=0 || clip.top!=0 || clip.right!=1280 || clip.bottom!=720) {
                std::cerr << "Untransformed scissor clipped the surface\n";
                ++failures;
            }
        } else {
            const auto clip=MakeScissor(regs,0);
            assert(clip.left==300 && clip.right==500);
            assert(clip.top==(lower_left ? 320 : 200));
            assert(clip.bottom==(lower_left ? 520 : 400));
        }
    }
    return failures ? 1 : 0;
}
"""
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-origin-") as temp:
            source = Path(temp) / "origin.cpp"
            binary = Path(temp) / "origin"
            source.write_text(driver)
            subprocess.run(
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
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
