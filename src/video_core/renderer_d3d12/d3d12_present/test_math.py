# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host validation of presentation coordinates and kernels without Windows headers."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DRIVER = r"""
#include <cassert>
#include <cmath>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_present/present_math.h"
using namespace D3D12::Present;
bool near(float a, float b) { return std::abs(a-b) < 1e-5f; }
template <class F> void rejects(F function) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
int main() {
    // Guest crops are normalized once; scaled cache images preserve these UVs.
    auto crop = NormalizeCrop({160,90,1120,630}, {1280,720});
    assert(near(crop.left,.125f) && near(crop.right,.875f));
    auto raw = EasuMapping({1280,720}, {3840,2160}, crop);
    auto scaled = EasuMapping({2560,1440}, {3840,2160}, crop);
    assert(near(raw[0],.25f) && near(scaled[0],.5f));
    assert(near(raw[2],159.625f) && near(scaled[2],319.75f));
    auto identity = EasuMapping({1280,720},{1280,720},{});
    assert(identity[0]==1 && identity[1]==1 && identity[2]==0 && identity[3]==0);
    constexpr std::array<std::array<float,2>,8> transformed{{
        {.25f,.5f},{.75f,.5f},{.25f,.5f},{.75f,.5f},
        {.5f,.75f},{.5f,.75f},{.5f,.25f},{.5f,.25f}}};
    for (unsigned flags=0; flags<8; ++flags)
        assert(TransformUv({.25f,.5f}, flags)==transformed[flags]);
    auto rotated = TransformUv({0,0},4);
    assert(rotated[0]==0 && rotated[1]==1);
    assert(TransformUv({0,0},3)==(std::array<float,2>{1,1}));
    assert(SyncInterval(false)==0 && SyncInterval(true)==1 && SyncInterval(true,4)==4);
    rejects([]{ SyncInterval(true,0); });
    rejects([]{ SyncInterval(false,5); });
    ValidateExtent({3840,2160},true);
    rejects([]{ ValidateExtent({3841,2160},true); });
    rejects([]{ ValidateExtent({0,1}); });
    rejects([]{ NormalizeCrop({0,0,1290,720},{1280,720}); });
    rejects([]{ NormalizeCrop({0,0,0,720},{1280,720}); });
    rejects([]{ NormalizeCrop({NAN,0,1,1},{1,1}); });
    assert(RcasStrength(0)==1 && RcasStrength(1)==.5f);
    assert(RcasStrength(-1)==1 && RcasStrength(2)==.5f);
    rejects([]{ RcasStrength(NAN); });
    // Catmull-Rom preserves constant colors and interpolates exact centers.
    for (unsigned i=0; i<=100; ++i) {
        auto w = CubicWeights(float(i)/100);
        assert(near(w[0]+w[1]+w[2]+w[3],1));
    }
    assert(CubicWeights(0)==(std::array<float,4>{0,1,0,0}));
    // Box filter: single texel, integral boundaries, negative edge and downscale.
    for (auto interval : {std::array<float,2>{.2f,.4f}, {0,4}, {-.25f,.25f}, {.25f,3.75f}}) {
        float total=0;
        for (int x=-2;x<6;++x) total+=AreaOverlap(interval[0],interval[1],x);
        assert(near(total,interval[1]-interval[0]));
    }
    assert(near(AreaOverlap(.2f,.4f,0),.2f));
}
"""


class PresentMathTests(unittest.TestCase):
    def test_coordinates_kernels_and_pacing(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-present-") as directory:
            source = Path(directory) / "math.cpp"
            binary = Path(directory) / "math"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)
