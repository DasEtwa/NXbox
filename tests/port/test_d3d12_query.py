# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run stream output layout and query arithmetic without the Windows SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_query_helpers.h"
using namespace D3D12;
template <typename F> void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
int main() {
    assert(MakeStreamOutputLayout({}).elements.empty());
    const std::array varyings{
        StreamOutputVarying{36, 2, 3, 32, 16, 4},
        StreamOutputVarying{33, 2, 3, 32, 4, 2},
        StreamOutputVarying{28, 0, 0, 24, 0, 4},
    };
    const auto layout = MakeStreamOutputLayout(varyings);
    assert(layout.strides[0] == 24 && layout.strides[2] == 32);
    assert(layout.strides[1] == 0 && layout.streams[2] == 3);
    assert(layout.elements.size() == 6);
    assert(layout.elements[0].attribute == 28 && layout.elements[0].components == 4);
    assert(layout.elements[1].attribute == StreamOutputElement::Gap);
    assert(layout.elements[1].components == 2 && layout.elements[1].buffer == 0);
    assert(layout.elements[2].attribute == StreamOutputElement::Gap);
    assert(layout.elements[2].components == 1 && layout.elements[2].buffer == 2);
    assert(layout.elements[3].attribute == 33 && layout.elements[3].start_component == 1);
    assert(layout.elements[4].attribute == StreamOutputElement::Gap);
    assert(layout.elements[4].components == 1);
    assert(layout.elements[5].attribute == 36);
    auto bad = varyings;
    bad[1].offset = 20; // Overlap with the other varying in buffer 2.
    rejects([&] { MakeStreamOutputLayout(bad); });
    bad = varyings; bad[1].stream = 2;
    rejects([&] { MakeStreamOutputLayout(bad); });
    bad = varyings; bad[1].stride = 36;
    rejects([&] { MakeStreamOutputLayout(bad); });
    for (const auto v : {
        StreamOutputVarying{32, 4, 0, 16, 0, 4},
        StreamOutputVarying{32, 0, 4, 16, 0, 4},
        StreamOutputVarying{35, 0, 0, 16, 0, 2},
        StreamOutputVarying{32, 0, 0, 16, 2, 1},
        StreamOutputVarying{32, 0, 0, 16, 12, 2},
        StreamOutputVarying{32, 0, 0, 2052, 0, 1},
        StreamOutputVarying{256, 0, 0, 16, 0, 1},
        StreamOutputVarying{32, 0, 0, 16, 0, 0},
        StreamOutputVarying{32, 0, 0, 16, UINT32_MAX, 1},
    }) {
        rejects([&] { MakeStreamOutputLayout(std::span(&v, 1)); });
    }
    std::vector<StreamOutputVarying> too_many;
    for (unsigned i = 0; i < 33; ++i)
        too_many.push_back({i * 4, 0, 0, 528, i * 16, 4});
    rejects([&] { MakeStreamOutputLayout(too_many); });
    too_many.pop_back();
    assert(!MakeStreamOutputLayout(too_many).elements.empty());
    // A sparse capture preserves a stride much larger than its output vector.
    const StreamOutputVarying sparse{32, 0, 0, 2048, 2000, 1};
    const auto gaps = MakeStreamOutputLayout(std::span(&sparse, 1));
    assert(gaps.elements[0].components == 500 && gaps.elements[2].components == 11);
    static_assert(CounterDifference(100, 40) == 60);
    static_assert(CounterDifference(2, 40) == 0);
    static_assert(StreamOutputOverflow(100, 40) == 60);
    static_assert(StreamOutputOverflow(40, 100) == 0);
    assert(TimestampNanoseconds(123456789, 10000000) == 12345678900ULL);
    assert(TimestampNanoseconds(1, 3) == 333333333);
    rejects([] { TimestampNanoseconds(1, 0); });
    assert(TimestampNanoseconds(UINT64_MAX - 1, UINT64_MAX) == 999999999);
    assert(TimestampNanoseconds(UINT64_MAX, UINT64_MAX) == 1000000000);
    // Compare the portable implementation to a wide-integer oracle, including
    // fractional products that overflow u64 and near-integral fractions.
    std::uint64_t seed = 42;
    for (unsigned i = 0; i < 10000; ++i) {
        seed = seed * 6364136223846793005ULL + 1;
        const auto ticks = seed;
        seed = seed * 6364136223846793005ULL + 1;
        const auto frequency = seed | 1;
        const auto expected = static_cast<std::uint64_t>(
            static_cast<__uint128_t>(ticks) * 1000000000ULL / frequency);
        assert(TimestampNanoseconds(ticks, frequency) == expected);
    }
}
"""


class D3D12QueryTests(unittest.TestCase):
    def test_stream_output_layout_and_query_arithmetic(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-query-") as temp:
            source = Path(temp) / "query.cpp"
            binary = Path(temp) / "query"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
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
