# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise compute geometry and the public constant ABI without a Windows SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_compute_pass.h"
using namespace D3D12;
template<class Exception, class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const Exception&) { rejected = true; }
    assert(rejected);
}
int main() {
    static_assert(ComputeRootDwords == 16);
    static_assert(static_cast<unsigned>(ComputeRootParameter::Constants) == 0);
    static_assert(static_cast<unsigned>(ComputeRootParameter::Cbv) == 1);
    static_assert(static_cast<unsigned>(ComputeRootParameter::Srv) == 2);
    static_assert(static_cast<unsigned>(ComputeRootParameter::Uav) == 3);
    static_assert(static_cast<unsigned>(ComputeRootParameter::Sampler) == 4);
    constexpr auto astc = AstcDecodeParameters{12, 10, 4096, 2048, 11, 2, 3}.Constants();
    static_assert(astc[0] == 12 && astc[1] == 10 && astc[2] == 4096);
    static_assert(astc[3] == 2048 && astc[4] == 11 && astc[5] == 2 && astc[6] == 3);
    static_assert(astc[7] == 0 && astc[15] == 0);
    constexpr auto unswizzle = UnswizzleParameters{
        {2,3,4}, 4, {-1,6,7}, 8192, 4096, 12, 3, 7, 2, 3}.Constants();
    static_assert(unswizzle[0] == 2 && unswizzle[1] == 3 && unswizzle[2] == 4);
    static_assert(unswizzle[3] == 4 && unswizzle[4] == UINT32_MAX);
    static_assert(unswizzle[5] == 6 && unswizzle[6] == 7 && unswizzle[7] == 8192);
    static_assert(unswizzle[8] == 4096 && unswizzle[9] == 12);
    static_assert(unswizzle[10] == 3 && unswizzle[11] == 7);
    static_assert(unswizzle[12] == 2 && unswizzle[13] == 3);
    static_assert(unswizzle[14] == 0 && unswizzle[15] == 0);
    // Odd mip blocks and array tails must be covered without a missing group.
    assert((HelperGroupCount(HelperComputePass::DecodeAstc, {9,17,3}) == ComputeGroups{2,3,3}));
    assert((HelperGroupCount(HelperComputePass::Unswizzle3DBcn, {9,7,5}) == ComputeGroups{2,1,2}));
    assert((HelperGroupCount(HelperComputePass::Unswizzle3D, {9,7,5}) == ComputeGroups{2,1,5}));
    assert((HelperGroupCount(HelperComputePass::Uint8ToUint16, {513,1,1}) == ComputeGroups{2,1,1}));
    assert((HelperGroupCount(HelperComputePass::QuadIndexed, {257,1,1}) == ComputeGroups{2,1,1}));
    assert(HasComputeWork(HelperGroupCount(HelperComputePass::QueriesPrefix, {2048,1,1})));
    for (auto pass : {HelperComputePass::DecodeAstc, HelperComputePass::Unswizzle2D,
                     HelperComputePass::Unswizzle3D, HelperComputePass::Unswizzle3DBcn,
                     HelperComputePass::Uint8ToUint16, HelperComputePass::QuadIndexed,
                     HelperComputePass::QueriesPrefix})
        assert(!HasComputeWork(HelperGroupCount(pass, {0,1,1})));
    assert((ComputeGroupCount({65535*8,1,1}, {8,1,1}) == ComputeGroups{65535,1,1}));
    rejects<std::out_of_range>([]{ ComputeGroupCount({65535*8+1,1,1}, {8,1,1}); });
    rejects<std::out_of_range>([]{ ComputeGroupCount({UINT32_MAX,1,1}, {8,1,1}); });
    rejects<std::out_of_range>([]{ HasComputeWork({0,65536,1}); });
    rejects<std::invalid_argument>([]{ ComputeGroupCount({0,1,1}, {0,1,1}); });
    rejects<std::out_of_range>([]{ HelperGroupCount(HelperComputePass::QueriesPrefix, {2049,1,1}); });
    rejects<std::out_of_range>([]{ HelperGroupCount(HelperComputePass::Uint8ToUint16, {UINT32_MAX,1,1}); });
    rejects<std::invalid_argument>([]{ HelperGroupCount(HelperComputePass::Count, {1,1,1}); });
    // Every lane geometry covers the requested extent and at most one tail group.
    for (unsigned n=1; n<10000; ++n) {
        auto groups = HelperGroupCount(HelperComputePass::Uint8ToUint16, {n,1,1});
        auto covered = groups[0]*512;
        assert(covered >= n && covered-n < 512);
        groups = HelperGroupCount(HelperComputePass::DecodeAstc, {n,1,1});
        covered = groups[0]*8;
        assert(covered >= n && covered-n < 8);
    }
}
"""


class D3D12ComputeTests(unittest.TestCase):
    def test_dispatch_geometry_and_constant_abi(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-compute-") as temp:
            source = Path(temp) / "compute.cpp"
            binary = Path(temp) / "compute"
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
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)
