# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production UAV publication and view readback without a Windows SDK."""

import json
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import CACHE, compile_run

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    body = source.split(signature, 1)[1].split("\ntemplate <class P>", 1)[0]
    return signature.replace("TextureCache<P>", "Cache") + body


class ShaderImageWriteTests(unittest.TestCase):
    def test_actual_publication_and_subresource_writer(self):
        source = CACHE.read_text()
        methods = "\n".join(
            method(source, signature)
            for signature in (
                "void TextureCache<P>::MarkModification(ImageId id)",
                "void TextureCache<P>::MarkModification(ImageBase& image)",
                "void TextureCache<P>::PublishShaderImageWrites(",
                "void TextureCache<P>::BeginPassDumpImageWrite()",
                "void TextureCache<P>::RecordPassDumpImageWrite(",
            )
        )
        driver = r"""
#define FMT_HEADER_ONLY
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using GPUVAddr = u64;
using ImageId = unsigned;
using ImageViewId = unsigned;
constexpr unsigned NULL_IMAGE_VIEW_ID = 0;
namespace ImageFlagBits { constexpr unsigned GpuModified = 1; }
namespace VideoCore::Surface {
unsigned DefaultBlockWidth(int) { return 1; }
unsigned DefaultBlockHeight(int) { return 1; }
unsigned BytesPerBlock(int) { return 4; }
}
enum class ImageType { e1D, e2D, e3D };
namespace PassDump {
struct Config { bool enabled=true; u64 frame=1200; std::filesystem::path directory; } config;
const Config& GetConfig() { return config; }
const char* FormatName(int) { return "A8B8G8R8_UNORM"; }
}
struct Subresource { int base_level, base_layer, num_layers; };
struct BufferImageCopy {
    size_t buffer_offset, buffer_size;
    unsigned buffer_row_length, buffer_image_height;
    Subresource image_subresource;
    std::array<unsigned, 3> image_offset, image_extent;
};
struct Map { std::span<unsigned char> mapped_span; };
struct ImageBase { unsigned flags=0; u64 modification_tick=0; };
struct Image : ImageBase {
    struct Info {
        struct Size { unsigned width=8, height=8; } size;
        int format=0, num_samples=1;
        ImageType type=ImageType::e2D;
    } info;
    GPUVAddr gpu_addr=23033741312;
    void DownloadMemory(Map map, const std::array<BufferImageCopy, 1>& copies) {
        const auto& c = copies[0];
        assert(c.image_subresource.num_layers == 1);
        assert(c.buffer_size == c.image_extent[0] * c.image_extent[1] * 4);
        std::fill(map.mapped_span.begin(), map.mapped_span.end(),
                  c.image_subresource.base_level * 16 + c.image_subresource.base_layer);
    }
};
struct View {
    ImageId image_id=1;
    bool buffer=false;
    int format=0;
    struct Range {
        struct Base { int level=1, layer=2; } base;
        struct Extent { int levels=2, layers=2; } extent;
    } range;
    bool IsBuffer() const { return buffer; }
};
struct Runtime {
    std::vector<unsigned char> bytes;
    unsigned finishes=0;
    Map DownloadStagingBuffer(size_t size) { bytes.resize(size); return {bytes}; }
    void Finish() { ++finishes; }
};
struct Cache {
    std::array<Image, 4> slot_images;
    std::array<View, 5> slot_image_views;
    Runtime runtime;
    u64 frame=1200, modification_tick=0, pass_dump_sequence=0;
    struct Target {
        ImageId id; u64 sequence; unsigned slot;
        s32 level=0, layer=0, levels=1, layers=1;
    };
    std::vector<Target> pass_dump_targets;
    std::vector<std::string> reports;
    unsigned flushes=0;
    void DumpPassRenderTargets() { ++flushes; }
    u64 CaptureFrame() const { return frame; }
    void ReportPassDump(const std::string& message) { reports.push_back(message); }
    void MarkModification(ImageId) noexcept;
    void MarkModification(ImageBase&) noexcept;
    void PublishShaderImageWrites(std::span<const ImageViewId>, std::string_view, u64);
    void BeginPassDumpImageWrite();
    void RecordPassDumpImageWrite(ImageViewId, std::string_view, GPUVAddr, u64);
};
"""
        driver += (
            methods
            + r"""
int main(int argc, char** argv) {
    PassDump::config.directory = std::filesystem::path{argv[1]} / "capture";
    Cache cache;
    cache.slot_image_views[2].image_id = 2;
    cache.slot_image_views[3].buffer = true;
    const std::array<ImageViewId, 4> writes{0, 1, 2, 3};
    // Coherence is independent of capture enablement. Both images advance past an
    // alias's old tick, so SynchronizeAliases' newer-writer check can select them.
    PassDump::config.enabled = false;
    cache.PublishShaderImageWrites(writes, "compute_uav", 17);
    assert(cache.slot_images[1].flags & ImageFlagBits::GpuModified);
    assert(cache.slot_images[2].modification_tick > cache.slot_images[1].modification_tick);
    assert(cache.modification_tick == 2 && cache.runtime.finishes == 0);
    assert(!std::filesystem::exists(PassDump::config.directory));
    PassDump::config.enabled = true;
    cache.frame = 1199;
    cache.BeginPassDumpImageWrite();
    assert(cache.flushes == 0);
    cache.frame = 1200;
    cache.BeginPassDumpImageWrite();
    assert(cache.flushes == 1);
    cache.PublishShaderImageWrites(std::span{writes}.subspan(1, 1), "compute_uav", 17);
    assert(cache.modification_tick == 3 && cache.runtime.finishes == 4);
    assert(cache.pass_dump_targets.size() == 1 && cache.pass_dump_targets[0].sequence == 1);
    assert(cache.pass_dump_targets[0].level == 1 && cache.pass_dump_targets[0].layer == 2);
    assert(cache.pass_dump_targets[0].levels == 2 && cache.pass_dump_targets[0].layers == 2);
    // Unsupported volumes are recorded explicitly, never as a zero-filled raw.
    cache.slot_images[1].info.type = ImageType::e3D;
    cache.RecordPassDumpImageWrite(1, "graphics_uav", 0, 0);
    assert(cache.runtime.finishes == 4);
    // Failures use the diagnostic sink and do not publish a successful producer.
    std::filesystem::create_directory(PassDump::config.directory / "blocked");
    PassDump::config.directory /= "manifest.jsonl";
    cache.RecordPassDumpImageWrite(1, "compute_uav", 0, 0);
    assert(cache.reports.back().find("PASS_DUMP image_write failed") == 0);
}
"""
        )
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "fmt"], text=True))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compile_run(driver, root, extra_flags=flags)
            entries = [
                json.loads(line)
                for line in (root / "capture/manifest.jsonl").read_text().splitlines()
            ]
            self.assertEqual(len(entries), 8)
            for entry in entries[:4]:
                self.assertEqual(entry["writer"], "compute_uav")
                self.assertEqual(entry["program"], 17)
                self.assertEqual(entry["gpu_addr"], 23033741312)
                level, layer = entry["level"], entry["layer"]
                self.assertEqual((entry["width"], entry["height"]), (8 >> level, 8 >> level))
                raw = (root / "capture" / entry["file"]).read_bytes()
                self.assertEqual(raw, bytes([level * 16 + layer]) * entry["bytes"])
            self.assertEqual(
                {(e["level"], e["layer"]) for e in entries[:4]}, {(1, 2), (1, 3), (2, 2), (2, 3)}
            )
            for entry in entries[4:]:
                self.assertEqual(entry["skipped"], "volume_or_msaa")
                self.assertEqual(entry["bytes"], 0)
                self.assertEqual(entry["file"], "")

    def test_d3d12_publishes_only_after_submitted_work(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        graphics = source.split("void RasterizerD3D12::PrepareDraw(", 1)[1].split(
            "void RasterizerD3D12::TraceUniformWord", 1
        )[0]
        compute = source.split("void RasterizerD3D12::DispatchCompute()", 1)[1].split(
            "void RasterizerD3D12::", 1
        )[0]
        for body in (graphics, compute):
            self.assertIn(
                "if (desc.is_written && view_id != VideoCommon::NULL_IMAGE_VIEW_ID)", body
            )
            self.assertLess(
                body.index("trace_submitted = true;"), body.index("PublishShaderImageWrites(")
            )
        self.assertLess(
            compute.index("HasComputeWork(groups)"), compute.index("host_compute_submit")
        )
        self.assertLess(
            compute.index("BeginPassDumpImageWrite();"), compute.index("compute_dispatch.Direct(")
        )


if __name__ == "__main__":
    unittest.main()
