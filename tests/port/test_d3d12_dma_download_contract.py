# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the shared texture cache's two-destination DMA call without the SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
using u8=std::uint8_t;
constexpr int D3D12_HEAP_TYPE_DEFAULT=0;
namespace VideoCommon { struct BufferImageCopy {}; }
namespace VideoCore::Surface { bool IsPixelFormatASTC(int) { return false; } }
struct Resource {
    int heap{};
    std::array<u8,64> bytes{};
    std::span<u8> mapped{bytes};
};
using ResourcePtr=std::shared_ptr<Resource>;
struct StagingBufferRef { ResourcePtr buffer; size_t offset; std::span<u8> mapped_span; };
struct Image {
    struct { bool converted_astc=false; } host_format;
    struct { int format{}; } info;
    ResourcePtr captured_destination;
    StagingBufferRef captured_readback;
    size_t captured_offset{};
    void DownloadMemory(std::span<const ResourcePtr>,std::span<const size_t>,
                        std::span<const VideoCommon::BufferImageCopy>);
    void Transfer(const StagingBufferRef& map,std::span<const VideoCommon::BufferImageCopy>,
                  bool download,const ResourcePtr& destination,size_t offset) {
        assert(download);
        captured_destination=destination;
        captured_readback=map;
        captured_offset=offset;
    }
};
__DOWNLOAD__
int main() {
    Image image;
    const auto gpu=std::make_shared<Resource>();
    gpu->mapped={}; // DEFAULT buffers have no CPU mapping.
    const auto readback=std::make_shared<Resource>();
    readback->heap=1;
    std::array buffers{gpu,readback};
    std::array<size_t,2> offsets{3,16};
    std::array<VideoCommon::BufferImageCopy,1> copies{};
    image.DownloadMemory(buffers,offsets,copies);
    assert(image.captured_destination==gpu && image.captured_offset==3);
    assert(image.captured_readback.buffer==readback && image.captured_readback.offset==16);
    assert(image.captured_readback.mapped_span.data()==readback->bytes.data()+16);
    assert(image.captured_readback.mapped_span.size()==48);
    bool rejected=false;
    offsets[1]=65;
    try { image.DownloadMemory(buffers,offsets,copies); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
}
"""


class D3D12DmaDownloadContractTests(unittest.TestCase):
    def test_array_call_retains_default_and_readback_destinations(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        marker = "void Image::DownloadMemory(std::span<const ResourcePtr>"
        implementation = marker + source.split(marker, 1)[1].split("void Image::Transfer", 1)[0]
        with tempfile.TemporaryDirectory(prefix="nxbox-dma-download-") as folder:
            cpp = Path(folder) / "dma.cpp"
            binary = Path(folder) / "dma"
            cpp.write_text(DRIVER.replace("__DOWNLOAD__", implementation))
            result = subprocess.run(
                ["clang++", "-std=c++20", str(cpp), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
