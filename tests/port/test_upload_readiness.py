# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute upload visibility and readiness guards from the maintained sources."""

import ast
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety
from test_mesa_full_chain import ROOT, SOURCE


class UploadReadinessTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory(prefix="nxbox-upload-test-") as temporary:
            runner = safety.MesaRenderSafetyTests()
            runner.root = Path(temporary)
            runner.compile_run(code)

    def test_converted_flush_covers_all_mips_and_layers(self):
        source = (ROOT / "src/video_core/renderer_opengl/gl_texture_cache.cpp").read_text()
        flush = source.split("    const auto upload_size =", 1)[1].split(
            "    glPixelStorei(GL_UNPACK_ALIGNMENT", 1
        )[0]
        util = (ROOT / "src/video_core/texture_cache/util.cpp").read_text()
        map_size = (
            "u32 MapSizeBytes" + util.split("u32 MapSizeBytes", 1)[1].split("\nstatic_assert", 1)[0]
        )
        self.compile_run(
            r"""
#include <cassert>
#include <cstddef>
using u32=unsigned;
namespace NxboxUpload { enum class Event { ExpandedFlush }; unsigned expanded=0;
void Record(Event) { ++expanded; } }
enum ImageFlagBits { AcceleratedUpload=1, Converted=2 };
bool True(unsigned bits) { return bits; }
struct ImageBase { unsigned flags=0, guest_size_bytes=0,
    converted_size_bytes=0, unswizzled_size_bytes=0; };
namespace VideoCommon {
"""
            + map_size
            + r"""
}
constexpr unsigned GL_PIXEL_UNPACK_BUFFER=1;
size_t flushed_offset=0, flushed_size=0;
void glFlushMappedBufferRange(unsigned, size_t offset, size_t size) {
    flushed_offset=offset; flushed_size=size;
}
struct Image : ImageBase {
void Flush(size_t buffer_offset) {
    const auto upload_size =
"""
            + flush
            + r"""
}
};
int main() {
 Image image;
 // Three mips, two layers: 64x64 + 32x32 + 16x16 texels, RGBA8.
 image.unswizzled_size_bytes=(4096+1024+256)*2; // ASTC 4x4: 1 byte/texel.
 image.converted_size_bytes=image.unswizzled_size_bytes*4;
 image.guest_size_bytes=16384;
 image.flags=Converted; image.Flush(256);
 assert(flushed_offset==256 && flushed_size==43008 && NxboxUpload::expanded==1);
 image.flags=0; image.Flush(512);
 assert(flushed_offset==512 && flushed_size==10752 && NxboxUpload::expanded==1);
 image.flags=AcceleratedUpload; image.Flush(0);
 assert(flushed_size==16384);
 // Recompressed CPU output can be smaller than the original unswizzled input.
 image.flags=Converted; image.converted_size_bytes=5376; image.Flush(64);
 assert(flushed_offset==64 && flushed_size==5376);
}
"""
        )

    def test_counter_gating_and_reporting(self):
        helper = (ROOT / "src/common/nxbox_upload_diag.h").read_text().replace("#pragma once", "")
        code = (
            r"""
#include <cassert>
#include <string>
unsigned publications=0;
std::string report;
void SetEnvironmentVariableA(const char* name, const char* value) {
 assert(std::string(name)=="NXBOX_MESA_UPLOAD_READY");
 report=value; ++publications;
}
#define NXBOX_UPLOAD_DIAG_MESA
"""
            + helper
            + r"""
int main() {
 const bool enabled=NxboxUpload::Enabled();
 NxboxUpload::Record(NxboxUpload::Event::AstcPendingBinding);
 NxboxUpload::Record(NxboxUpload::Event::PipelineSkipped);
 NxboxUpload::Record(NxboxUpload::Event::StagingBusyMap);
 NxboxUpload::Record(NxboxUpload::Event::BufferInvalidation);
 if (enabled) {
  assert(report.find("astc_pending_binding=1")!=std::string::npos);
  assert(report.find("pipeline_skipped=1")!=std::string::npos);
  assert(report.find("staging_busy_map=1")!=std::string::npos);
  assert(report.find("buffer_invalidation=1")!=std::string::npos);
  for(unsigned i=0;i<8192;++i) NxboxUpload::Publish();
  assert(publications==18);
 } else { assert(publications==0); }
 assert(NxboxUpload::ForceAstcSync());
}
"""
        )
        old_diag = os.environ.get("NXBOX_UPLOAD_DIAG")
        old_sync = os.environ.get("NXBOX_ASTC_SYNC")
        try:
            os.environ["NXBOX_ASTC_SYNC"] = "1"
            for value in ("0", "1", "10"):
                os.environ["NXBOX_UPLOAD_DIAG"] = value
                self.compile_run(code)
        finally:
            for key, value in (("NXBOX_UPLOAD_DIAG", old_diag), ("NXBOX_ASTC_SYNC", old_sync)):
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

    def test_sync_policy_after_title_settings(self):
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        title = source.index("ApplyEdenSettingsFile(TitleSettings::File(local_state, title))")
        sync = source.index("if (NxboxUpload::ForceAstcSync())")
        load = source.index("system.Load(window, path, parameters)")
        self.assertLess(title, sync)
        self.assertLess(sync, load)
        self.assertIn('collect("NXBOX_GL_UPLOAD_READY",', source)
        self.assertIn('collect("NXBOX_MESA_UPLOAD_READY",', source)

    @unittest.skipUnless(SOURCE.is_dir(), "Provide pristine /tmp/mesa-pin")
    def test_temporary_staging_guard_and_patch_invocation(self):
        path = ROOT / "tools/nxbox/patch_mesa_uwp.py"
        tree = ast.parse(path.read_text())
        patch = next(
            node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "patch"
        )
        calls = [
            node.func.id
            for node in ast.walk(patch)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
        ]
        self.assertEqual(calls.count("patch_upload_ready"), 1)
        spec = importlib.util.spec_from_file_location("upload_patch", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(prefix="nxbox-upload-patch-") as temporary:
            root = Path(temporary)
            driver = root / "src/gallium/drivers/d3d12"
            driver.mkdir(parents=True)
            shutil.copyfile(
                SOURCE / "src/gallium/drivers/d3d12/d3d12_resource.cpp",
                driver / "d3d12_resource.cpp",
            )
            module.patch_buffer_staging(root)
            module.patch_upload_ready(root)
            source = (driver / "d3d12_resource.cpp").read_text()
            guard = source.split("          resource_is_busy(ctx, staging_res, true))", 1)[1].split(
                "   }\n\n   pipe_resource_reference(&ptrans->resource", 1
            )[0]
            self.assertEqual(
                (driver / "nxbox_upload_diag.h").read_text(),
                (ROOT / "src/common/nxbox_upload_diag.h").read_text(),
            )
            self.compile_run(
                r"""
#include <cassert>
constexpr unsigned PIPE_MAP_WRITE=1;
namespace NxboxUpload { enum class Event { StagingBusyMap }; unsigned busy=0;
void Record(Event) { ++busy; } }
unsigned waits=0;
struct Resource { bool in_flight=false; int bo=1; };
void d3d12_resource_wait_idle(void*, Resource* resource, bool write) {
 assert(write && resource->in_flight); resource->in_flight=false; ++waits;
}
Resource* active;
void* d3d12_bo_map(int, int*) { assert(!active->in_flight); return active; }
void map(Resource* staging_res, unsigned usage) {
 void* ctx=nullptr; int range=0; void* ptr=nullptr; active=staging_res;
 if ((usage & PIPE_MAP_WRITE) && staging_res->in_flight)
"""
                + guard
                + r"""
 assert(ptr==staging_res);
}
int main() {
 Resource resource; map(&resource,PIPE_MAP_WRITE); assert(waits==0);
 resource.in_flight=true; map(&resource,PIPE_MAP_WRITE);
 assert(waits==1 && NxboxUpload::busy==1);
 map(&resource,0); assert(waits==1);
}
"""
            )
