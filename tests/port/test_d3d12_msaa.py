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
