# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production buffer-alias classifier with image-buffer bindings."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class DbszBufferAlias(unittest.TestCase):
    def test_image_buffers_are_uavs_even_when_read_only(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        start = source.index("bool IsStorageBuffer(")
        classifier = source[start : source.index("\n}", start) + 2]
        descriptor = (D3D12 / "d3d12_descriptor.h").read_text()
        start = descriptor.index("struct BoundBuffer {")
        bound_buffer = descriptor[start : descriptor.index("\n};", start) + 3]
        runtime = (D3D12 / "d3d12_buffer_cache.cpp").read_text()
        start = runtime.index("void BufferCacheRuntime::BindImageBuffer(")
        bind_image = runtime[start : runtime.index("\n}", start) + 2]
        driver = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
using u32 = std::uint32_t;
using ResourcePtr = std::shared_ptr<int>;
namespace VideoCore::Surface { enum class PixelFormat { R32 }; }
__BUFFER__
struct Stage { std::vector<BoundBuffer> storage, texel; };
struct BindingState { std::array<Stage, 5> stages; unsigned current_stage{}; };
struct Buffer { ResourcePtr resource; ResourcePtr Handle() const { return resource; } };
struct BufferCacheRuntime {
    BindingState bindings;
    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format) {
        bindings.stages[bindings.current_stage].texel.push_back(
            {buffer.Handle(), offset, size, false, static_cast<u32>(format)});
    }
    void BindImageBuffer(Buffer&, u32, u32, VideoCore::Surface::PixelFormat);
};
__BIND__
__CLASSIFIER__
int main() {
    BufferCacheRuntime runtime;
    Buffer input{std::make_shared<int>(1)}, image{std::make_shared<int>(2)};
    runtime.BindTextureBuffer(input, 0, 256, VideoCore::Surface::PixelFormat::R32);
    assert(!IsStorageBuffer(runtime.bindings, input.resource));
    runtime.BindImageBuffer(image, 256, 512, VideoCore::Surface::PixelFormat::R32);
    assert(IsStorageBuffer(runtime.bindings, image.resource));
    assert(!runtime.bindings.stages[0].texel[1].written);
    assert(runtime.bindings.stages[0].texel[1].offset == 256);
    assert(runtime.bindings.stages[0].texel[1].size == 512);
    runtime.bindings.stages[4].storage.push_back({input.resource});
    assert(IsStorageBuffer(runtime.bindings, input.resource));
    assert(!IsStorageBuffer(runtime.bindings, std::make_shared<int>(3)));
    runtime.bindings.stages[0].texel.clear();
    assert(!IsStorageBuffer(runtime.bindings, image.resource));
}
"""
        driver = driver.replace("__BUFFER__", bound_buffer)
        driver = driver.replace("__BIND__", bind_image)
        driver = driver.replace("__CLASSIFIER__", classifier)
        with tempfile.TemporaryDirectory(prefix="nxbox-dbsz-alias-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(driver)
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
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_runtime_preserves_image_identity_and_both_paths_copy_constants(self):
        header = (D3D12 / "d3d12_buffer_cache.h").read_text()
        self.assertIn("SEPARATE_IMAGE_BUFFER_BINDINGS = true;", header)
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        self.assertEqual(
            source.count("if (buffer.resource && IsStorageBuffer(bindings, buffer.resource))"), 2
        )
        self.assertEqual(source.count("buffer = buffer_runtime.CopyUniform(buffer);"), 2)


if __name__ == "__main__":
    unittest.main()
