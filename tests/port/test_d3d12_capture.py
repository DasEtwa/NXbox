# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise shader deduplication and the actual presentation-manifest writer on the host."""

import importlib.util
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D = ROOT / "src/video_core/renderer_d3d12"
CACHE = ROOT / "src/video_core/texture_cache/texture_cache.h"
SPEC = importlib.util.spec_from_file_location("pass_diff", ROOT / "tools/nxbox/pass_diff.py")
pass_diff = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pass_diff)


def compile_run(source, directory, environment=None, extra_flags=()):
    source_file = directory / "driver.cpp"
    executable = directory / "driver"
    source_file.write_text(source)
    subprocess.run(
        [
            "clang++",
            "-std=c++20",
            "-pthread",
            *extra_flags,
            "-I",
            str(directory),
            "-I",
            str(ROOT / "src"),
            str(source_file),
            "-o",
            str(executable),
        ],
        check=True,
        capture_output=True,
        text=True,
        env=environment,
    )
    subprocess.run(
        [str(executable), str(directory)],
        check=True,
        capture_output=True,
        text=True,
        env=environment,
    )
    return executable


class CaptureTests(unittest.TestCase):
    def test_hlsl_once_across_workers_and_specializations(self):
        driver = r"""
#include <cassert>
#include <thread>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_hlsl_dump.h"
int main(int argc, char** argv) {
    const std::filesystem::path root = argv[1];
    D3D12::HlslDump dump;
    std::vector<std::thread> workers;
    for (int i = 0; i < 16; ++i)
        workers.emplace_back([&] { dump.Write(root, 0xbd9a98ab639d4299ULL, 4, "first"); });
    for (auto& worker : workers) worker.join();
    const auto primary = root / "gfx_bd9a98ab639d4299_4.hlsl";
    const auto old = std::filesystem::last_write_time(primary) - std::chrono::hours{24};
    std::filesystem::last_write_time(primary, old);
    dump.Write(root, 0xbd9a98ab639d4299ULL, 4, "first");
    assert(std::filesystem::last_write_time(primary) == old);
    dump.Write(root, 0xbd9a98ab639d4299ULL, 4, "specialized");
    dump.Write(root, 0xbd9a98ab639d4299ULL, 4, "specialized");
    dump.Write(root, 0x6e918f49289dbac4ULL, 1, "vertex");
    dump.Write(root, 9, 5, "compute");
    bool failed = false;
    try { dump.Write(root / "missing", 10, 1, "retry"); }
    catch (const std::exception&) { failed = true; }
    assert(failed);
    std::filesystem::create_directory(root / "missing");
    dump.Write(root / "missing", 10, 1, "retry");
    assert(std::filesystem::exists(root / "missing/gfx_000000000000000a_1.hlsl"));
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            compile_run(driver, directory)
            shaders = sorted(directory.glob("gfx_*.hlsl"))
            self.assertEqual(len(shaders), 4)
            self.assertEqual(
                {p.read_text() for p in shaders}, {"first", "specialized", "vertex", "compute"}
            )
            for shader in shaders:
                hashes = shader.with_suffix(".hash").read_text()
                self.assertIn(f"guest_hash={shader.name.split('_')[1]}", hashes)
                self.assertIn("source_hash=", hashes)

    def test_prewarm_fresh_and_disabled_wiring(self):
        source = (D3D / "d3d12_pipeline_cache.cpp").read_text()
        wrapper = source.split("void DumpGraphicsHlsl(", 1)[1].split("// Diagnostic for array", 1)[
            0
        ]
        self.assertLess(wrapper.index("if (!enabled)"), wrapper.index("static HlslDump dump;"))
        self.assertLess(wrapper.index("if (!enabled)"), wrapper.index("dump.Write("))
        prewarm = source.split("void BuildPrewarmedPipeline(", 1)[1].split(
            "RecipeReader metadata", 1
        )[0]
        self.assertIn("DumpGraphicsHlsl(stage.hash, stage.stage, stage.hlsl);", prewarm)
        self.assertLess(prewarm.index("DumpGraphicsHlsl("), prewarm.index("compiler.Compile("))
        self.assertEqual(
            source.count("DumpGraphicsHlsl(guest_hashes[index], program.stage, sources[index]);"), 2
        )
        self.assertIn("DumpGraphicsHlsl(shader_hash, Shader::Stage::Compute, source);", source)
        self.assertEqual(source.count("DumpGraphicsHlsl(stage.hash, stage.stage, stage.hlsl);"), 3)

    def test_actual_presentation_writer_and_compare_reader(self):
        source = CACHE.read_text()
        method = source.split(
            "template <class P>\nstd::string TextureCache<P>::RecordPassDumpPresentation", 1
        )[1].split("\ntemplate <class P>", 1)[0]
        # Compile the production method with a tiny cache facade; no D3D12 SDK is needed.
        method = "std::string Cache::RecordPassDumpPresentation" + method
        driver = (
            r"""
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <sstream>
using u64 = std::uint64_t;
using DAddr = u64;
using GPUVAddr = u64;
#define LOG_ERROR(...) ((void)0)
// Minimal {} formatter sufficient for this method's JSON fields.
namespace fmt {
template<class... A> std::string format(std::string_view pattern, A... args) {
    std::vector<std::string> values;
    ([&] { std::ostringstream s; s << args; values.push_back(s.str()); }(), ...);
    std::string out; size_t index=0;
    for(size_t i=0;i<pattern.size();++i) {
        if(pattern.substr(i,2)=="{{") {out+='{';++i;}
        else if(pattern.substr(i,2)=="}}") {out+='}';++i;}
        else if(pattern.substr(i,2)=="{}") {out+=values.at(index++);++i;}
        else out+=pattern[i];
    }
    return out;
}
}
namespace PassDump {
struct Config { bool enabled=true; u64 frame=120; std::filesystem::path directory; } config;
const Config& GetConfig() { return config; }
const char* FormatName(int) { return "A8B8G8R8_UNORM"; }
}
struct Size { unsigned width=1920, height=1080; };
struct Info { int format=1; Size size; };
struct Image { GPUVAddr gpu_addr=0x55ceb0000ULL; Info info; };
struct ImageView { unsigned image_id=1; int format=1; Size size; };
struct Cache {
    u64 frame=120; bool pending=true; unsigned flushes=0;
    u64 pass_dump_sequence=72;
    std::vector<std::string> reports;
    void ReportPassDump(const std::string& message) { reports.push_back(message); }
    struct Target { unsigned id; u64 sequence; unsigned slot; };
    std::vector<Target> pass_dump_targets{{1, 12, 1}};
    std::vector<Image> slot_images{2};
    u64 CaptureFrame() { return frame; }
    void DumpPassRenderTargets() {
        if(pending) { pending=false; ++flushes; pass_dump_targets.push_back({1,72,0}); }
    }
    std::string RecordPassDumpPresentation(DAddr, const ImageView*);
};
"""
            + method
            + r"""
int main(int argc, char** argv) {
    PassDump::config.directory=argv[1]; Cache cache; ImageView view;
    PassDump::config.enabled=false;
    assert(cache.RecordPassDumpPresentation(0x26131000, &view).empty());
    assert(cache.flushes==0);
    PassDump::config.enabled=true; cache.frame=119;
    assert(cache.RecordPassDumpPresentation(0x26131000, &view).empty());
    assert(!std::filesystem::exists(PassDump::config.directory/"manifest.jsonl"));
    cache.frame=120;
    assert(!cache.RecordPassDumpPresentation(0x26131000, &view).empty());
    assert(cache.flushes==1);
    assert(!cache.reports.empty());
    assert(cache.reports.back().find("PASS_DUMP manifest frame=120 passes=72 dir=") == 0);
    cache.RecordPassDumpPresentation(0x26132000, nullptr);
    view.image_id=0;
    cache.RecordPassDumpPresentation(0x26133000, &view);
    PassDump::config.directory /= "manifest.jsonl";
    cache.RecordPassDumpPresentation(0x26134000, &view);
    assert(cache.reports.back().find("PASS_DUMP presentation failed dir=") == 0);
    cache.frame=121;
    assert(cache.RecordPassDumpPresentation(0x26131000, &view).empty());
}
"""
        )
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            compile_run(driver, directory)
            records = [
                json.loads(line) for line in (directory / "manifest.jsonl").read_text().splitlines()
            ]
            self.assertEqual(len(records), 3)
            presented, fallback, unmatched = records
            self.assertEqual(presented["frame"], 120)
            self.assertEqual(presented["cpu_addr"], 0x26131000)
            self.assertEqual(presented["gpu_addr"], 0x55CEB0000)
            self.assertEqual((presented["producer_seq"], presented["producer_slot"]), (72, 0))
            self.assertEqual(
                (presented["format"], presented["width"], presented["height"]),
                ("A8B8G8R8_UNORM", 1920, 1080),
            )
            self.assertEqual(fallback["source"], "guest_memory")
            self.assertEqual((fallback["gpu_addr"], fallback["producer_seq"]), (0, 0))
            self.assertEqual(unmatched["producer_seq"], 0)
            self.assertEqual(pass_diff.read_manifest(directory), [])

    def test_actual_raw_writer_closes_before_reporting_success(self):
        source = CACHE.read_text()
        download = source.split("    const auto download =", 1)[1].split("    try {", 1)[0]
        driver = (
            r"""
#define FMT_HEADER_ONLY
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include <fmt/format.h>
using u32 = std::uint32_t;
using s32 = std::int32_t;
namespace VideoCore::Surface {
unsigned DefaultBlockWidth(int) { return 1; }
unsigned DefaultBlockHeight(int) { return 1; }
unsigned BytesPerBlock(int) { return 4; }
}
struct Extent2D { unsigned width, height; };
struct Subresource { int base_level, base_layer, num_layers; };
struct BufferImageCopy {
    size_t buffer_offset, buffer_size;
    unsigned buffer_row_length, buffer_image_height;
    Subresource image_subresource;
    std::array<unsigned,3> image_offset, image_extent;
};
struct Map { std::span<unsigned char> mapped_span; };
struct Image {
    struct Info { struct Size { unsigned width=1, height=1; } size; int format=0; } info;
    void DownloadMemory(Map& map, const std::array<BufferImageCopy,1>& copies) {
        assert(copies[0].buffer_size==4);
        std::fill(map.mapped_span.begin(), map.mapped_span.end(), 76);
    }
};
struct Runtime {
    std::array<unsigned char,4> bytes{};
    bool finished=false;
    Map DownloadStagingBuffer(size_t size) { assert(size==4); return {bytes}; }
    void Finish() { finished=true; }
};
int main(int argc, char** argv) {
    Runtime runtime;
    Image image;
    const auto CaptureFrame = [] { return 1200; };
    const unsigned seq=1;
    std::vector<std::string> reports;
    const auto ReportPassDump = [&](const std::string& message) { reports.push_back(message); };
    const auto download ="""
            + download
            + r"""
    const std::filesystem::path root=argv[1];
    const auto output=root / "pass_0001_c0.raw";
    assert(download(image, 0, output)==4);
    assert(runtime.finished);
    assert(std::filesystem::file_size(output)==4);
    std::ifstream input{output, std::ios::binary};
    std::string bytes{std::istreambuf_iterator<char>{input}, {}};
    assert(bytes==std::string(4, 76));
    assert(reports.size()==1);
    assert(reports[0].find("PASS_DUMP wrote frame=1200 seq=1 bytes=4 file=")==0);
    bool rejected=false;
    try { download(image, 0, root / "missing" / "output.raw"); }
    catch (const std::exception&) { rejected=true; }
    assert(rejected);
    assert(reports.size()==1);
}
"""
        )
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "fmt"], text=True))
        with tempfile.TemporaryDirectory() as temporary:
            compile_run(driver, Path(temporary), extra_flags=flags)

    def test_destination_probe_writes_marker_and_reports_unwritable_paths(self):
        driver = r"""
#include <cassert>
#include <sstream>
#include "video_core/pass_dump.h"
int main(int argc, char** argv) {
    const std::filesystem::path root = argv[1];
    const auto destination = root / "LocalState" / "passdump";
    VideoCommon::PassDump::PrepareDirectory(destination, "1200");
    std::ifstream marker{destination / "capture.txt"};
    std::stringstream contents;
    contents << marker.rdbuf();
    assert(contents.str() == "NXBOX_PASS_DUMP_FRAME=1200\n");
    // A stale marker is replaced for the next session.
    VideoCommon::PassDump::PrepareDirectory(destination, "3000");
    std::ifstream updated{destination / "capture.txt"};
    std::string line;
    std::getline(updated, line);
    assert(line == "NXBOX_PASS_DUMP_FRAME=3000");
    bool rejected = false;
    try { VideoCommon::PassDump::PrepareDirectory(destination / "capture.txt", "1"); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
    // Directory creation can succeed even when opening the output fails.
    const auto blocked = root / "blocked";
    std::filesystem::create_directories(blocked / "capture.txt");
    rejected = false;
    try { VideoCommon::PassDump::PrepareDirectory(blocked, "1"); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            compile_run(driver, Path(temporary))

    def test_capture_reports_reach_the_runtime_sink_without_eden_logging(self):
        source = CACHE.read_text()
        method = source.split("template <class P>\nvoid TextureCache<P>::ReportPassDump", 1)[1]
        method = method.split("\ntemplate <class P>", 1)[0]
        driver = (
            r"""
#include <cassert>
#include <string>
#include <vector>
#define LOG_INFO(...) ((void)0)
struct Runtime {
    std::vector<std::string> messages;
    void ReportPassDump(const std::string& message) { messages.push_back(message); }
};
struct Cache { Runtime runtime; void ReportPassDump(const std::string&); };
"""
            + "void Cache::ReportPassDump"
            + method
            + r"""
int main() {
    Cache cache;
    cache.ReportPassDump("PASS_DUMP failed frame=1200 seq=1 error=denied");
    cache.ReportPassDump("PASS_DUMP wrote frame=1200 seq=2 bytes=4 file=pass_0002_c0.raw");
    assert(cache.runtime.messages.size() == 2);
    assert(cache.runtime.messages[0].find("error=denied") != std::string::npos);
    assert(cache.runtime.messages[1].find("bytes=4") != std::string::npos);
}
"""
        )
        with tempfile.TemporaryDirectory() as temporary:
            compile_run(driver, Path(temporary))

    def test_submission_trace_selection_and_event_limit(self):
        driver = r"""
#define FMT_HEADER_ONLY
#include <cassert>
#include <string>
#include <vector>
#include "video_core/submission_trace.h"
int main() {
    using namespace VideoCommon::SubmissionTrace;
    EndFrame(2);
    const bool expected = std::string_view{std::getenv("EXPECT_ACTIVE")} == "1";
    assert(Active() == expected);
    for (int i = 0; i < 20002; ++i) Log("guest_draw {}", i);
    EndFrame(3);
    assert(!Active());
    if (expected) {
        assert(messages.size() == 20002);
        assert(messages[20000].find("TRUNCATED limit=20000") != std::string::npos);
        assert(messages.back().find("frame_end events=20002 truncated=true") != std::string::npos);
    } else {
        assert(messages.empty());
        assert(events.load() == 0);
    }
    Reset();
    assert(frame.load() == 1);
    assert(events.load() == 0);
}
"""
        logger = r"""
#pragma once
#include <string>
#include <vector>
#include <fmt/format.h>
inline std::vector<std::string> messages;
template<class... Args> void Record(fmt::format_string<Args...> pattern, Args&&... args) {
    messages.push_back(fmt::format(pattern, std::forward<Args>(args)...));
}
#define LOG_INFO(category, ...) Record(__VA_ARGS__)
#define LOG_WARNING(category, ...) Record(__VA_ARGS__)
"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "common").mkdir()
            (directory / "common/logging.h").write_text(logger)
            environment = dict(
                os.environ,
                NXBOX_SUBMISSION_TRACE="1",
                NXBOX_D3D12_DRAW_TRACE_FRAME="2",
                EXPECT_ACTIVE="1",
            )
            # Discover installed host fmt headers without machine-specific paths.
            flags = []
            if shutil.which("pkg-config"):
                flags = shlex.split(
                    subprocess.check_output(["pkg-config", "--cflags", "fmt"], text=True)
                )
            executable = compile_run(driver, directory, environment, flags)
            for selected, enabled in (("2junk", "1"), ("-2", "1"), ("0", "1"), ("2", "0")):
                environment.update(
                    NXBOX_D3D12_DRAW_TRACE_FRAME=selected,
                    NXBOX_SUBMISSION_TRACE=enabled,
                    EXPECT_ACTIVE="0",
                )
                subprocess.run(
                    [str(executable)], check=True, capture_output=True, text=True, env=environment
                )

    def test_submission_trace_before_cpu_gate_and_gpu_predicate_readback(self):
        engine = (ROOT / "src/video_core/engines/draw_manager.cpp").read_text()
        for method, event in (
            ("ProcessDraw(", "guest_draw"),
            ("ProcessDrawIndirect(", "guest_indirect"),
        ):
            body = engine.split("void Maxwell3D::DrawManager::" + method, 1)[1]
            self.assertLess(body.index(event), body.index("if (maxwell3d.ShouldExecute())"))
        rasterizer = (D3D / "d3d12_rasterizer.cpp").read_text()
        self.assertLess(
            rasterizer.index("query_runtime.TracePredicate("),
            rasterizer.index("        draw(commands);"),
        )
        self.assertIn("u64 trace_draw = 0;", rasterizer)
        self.assertIn("trace_draw = ++draw_trace_lines;", rasterizer)
        self.assertIn("texture_cache.CapturePassSequence(), trace_draw,", rasterizer)
        self.assertIn("VideoCommon::SubmissionTrace::Reset();", rasterizer)
        self.assertIn("host_indirect args_gpu=", rasterizer)
        self.assertIn("host_compute_end submitted={}", rasterizer)
        query = (D3D / "d3d12_query_cache.cpp").read_text()
        trace = query.split("void QueryCacheRuntime::TracePredicate(", 1)[1].split(
            "QueryCacheRuntime::Timestamp", 1
        )[0]
        self.assertLess(
            trace.index("PauseHostConditionalRendering();"), trace.index("CopyBufferRegion(")
        )
        self.assertLess(
            trace.index("CopyBufferRegion("), trace.index("ResumeHostConditionalRendering();")
        )
        prepare = query.split("StagingBufferRef QueryCacheRuntime::PreparePredicateTrace()", 1)[
            1
        ].split("// Observe the same predicate bytes", 1)[0]
        self.assertIn("impl->readback.Request(8)", prepare)
        self.assertNotIn(".Request(", trace)
        self.assertLess(
            rasterizer.index("query_runtime.PreparePredicateTrace()"),
            rasterizer.index("const u64 table_tick = context.CurrentTick()"),
        )
        self.assertIn("AfterCompletion(", trace)
        self.assertNotIn(".Finish(", trace)
        self.assertIn("D3D12_PREDICATION_OP_EQUAL_ZERO ? value != 0 : value == 0", trace)
        self.assertIn("query_readback_wait_begin", query)
        self.assertIn("D3D12_QUERY_RESOLVED frame=", query)


if __name__ == "__main__":
    unittest.main()
