# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise list-restart conversion and frame diagnostic accounting on pinned Mesa."""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety
from test_mesa_full_chain import ROOT, SOURCE


@unittest.skipUnless(SOURCE.is_dir(), "Provide pristine /tmp/mesa-pin or NXBOX_MESA_SRC")
class DrawDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-draw-test-")
        cls.root = Path(cls.temp.name)
        cls.mesa = cls.root / "mesa"
        shutil.copytree(SOURCE, cls.mesa, ignore=shutil.ignore_patterns(".git"))
        environment = os.environ.copy()
        environment.pop("NXBOX_MESA_SKIP", None)
        subprocess.run(
            [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(cls.mesa)],
            env=environment,
            check=True,
            capture_output=True,
            timeout=60,
        )
        cls.driver = cls.mesa / "src/gallium/drivers/d3d12"

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def compile_run(self, code, args=()):
        runner = safety.MesaRenderSafetyTests()
        runner.root = self.root
        try:
            runner.compile_run(code, args)
        except subprocess.CalledProcessError as error:
            self.fail(error.stderr or str(error))

    def test_list_restart_routes_and_restarts_assembly(self):
        source = (self.driver / "d3d12_draw.cpp").read_text()
        predicate = source.split("   if (!prim_supported((enum mesa_prim)dinfo->mode) ||", 1)[1]
        predicate = "!prim_supported((enum mesa_prim)dinfo->mode) ||" + predicate.split(" {", 1)[0]
        predicate = predicate.rsplit(")", 1)[0]  # closing if, not predicate
        original = (SOURCE / "src/gallium/drivers/d3d12/d3d12_draw.cpp").read_text()
        primitive_support = (
            "static bool\nprim_supported"
            + original.split("static bool\nprim_supported", 1)[1].split("static inline", 1)[0]
        )
        context = (self.driver / "d3d12_context.cpp").read_text()
        restart_mask = context.split("cfg.restart_primtypes_mask = ", 1)[1].split(";", 1)[0]
        # Run the actual upstream restart scanner that primconvert now reaches.
        restart = (SOURCE / "src/gallium/auxiliary/util/u_prim_restart.c").read_text()
        restart = "struct range_info {" + restart.split("struct range_info {", 1)[1]
        restart = restart.split("/**\n * Implement primitive restart", 1)[0]
        restart = restart.replace("{ .min_index = UINT32_MAX, 0 }", "{}")
        code = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#define MALLOC(size) static_cast<pipe_draw_start_count_bias *>(malloc(size))
#define REALLOC(p,old_size,new_size) static_cast<pipe_draw_start_count_bias *>(realloc(p,new_size))
#define MIN2(a,b) std::min(a,b)
#define MAX2(a,b) std::max(a,b)
enum mesa_prim { MESA_PRIM_POINTS, MESA_PRIM_LINES, MESA_PRIM_LINE_STRIP,
 MESA_PRIM_TRIANGLES, MESA_PRIM_TRIANGLE_STRIP, MESA_PRIM_PATCHES, MESA_PRIM_QUADS,
 MESA_PRIM_LINES_ADJACENCY, MESA_PRIM_LINE_STRIP_ADJACENCY,
 MESA_PRIM_TRIANGLES_ADJACENCY, MESA_PRIM_TRIANGLE_STRIP_ADJACENCY };
struct pipe_draw_info { mesa_prim mode; unsigned index_size,restart_index; bool primitive_restart; };
struct pipe_draw_start_count_bias { unsigned start,count; int index_bias; };
bool u_trim_pipe_prim(mesa_prim mode,unsigned *count) {
 unsigned multiple=mode==MESA_PRIM_TRIANGLES?3:mode==MESA_PRIM_LINES?2:1;
 *count-=*count%multiple; return *count!=0;
}
"""
            + primitive_support
            + restart
            + "\nbool convert(pipe_draw_info *dinfo) { return "
            + predicate
            + "; }\n"
            + r"""
int main() {
 const unsigned strip_mask = """
            + restart_mask
            + r""";
 for(unsigned mode=0;mode<=MESA_PRIM_TRIANGLE_STRIP;++mode) {
  for(unsigned index_size: {2u,4u}) {
   const unsigned fixed=index_size==2?0xffff:0xffffffff;
   pipe_draw_info info{static_cast<mesa_prim>(mode),index_size,fixed,true};
   const bool strip=mode==MESA_PRIM_LINE_STRIP || mode==MESA_PRIM_TRIANGLE_STRIP;
   assert(convert(&info)==!strip); assert(bool(strip_mask&(1u<<mode))==strip);
   info.restart_index=7; assert(convert(&info));
   info.primitive_restart=false; assert(!convert(&info));
  }
 }
 // An incomplete triangle before restart must not borrow vertices from the next segment.
 uint16_t indices[]{0xffff,0,1,2,3,0xffff,4,5,6,7,8,9,0xffff};
 pipe_draw_info info{MESA_PRIM_TRIANGLES,2,0xffff,true};
 pipe_draw_start_count_bias draw{20,13,-4};
 unsigned n=0,min=0,max=0,total=0;
 auto *ranges=util_prim_restart_convert_to_direct(indices,&info,&draw,&n,&min,&max,&total);
 assert(n==2 && total==9);
 assert(ranges[0].start==21 && ranges[0].count==3 && ranges[0].index_bias==-4);
 assert(ranges[1].start==26 && ranges[1].count==6 && ranges[1].index_bias==-4);
 free(ranges);
}
"""
        )
        self.compile_run(code)

    def test_frame_counts_scope_exits_and_exact_gate(self):
        header = (self.driver / "nxbox_draw_diag.h").read_text().replace("#pragma once", "")
        code = (
            r"""
#include <cassert>
#include <map>
#include <string>
#include <cstring>
std::map<std::string,std::string> reports;
unsigned long GetEnvironmentVariableA(const char *key,char *value,unsigned long size) {
 auto text=reports[key]; if(text.size()<size) std::strcpy(value,text.c_str());
 return text.size();
}
void SetEnvironmentVariableA(const char *key,const char *value) { reports[key]=value; }
"""
            + header
            + r"""
void skip() { NxboxDrawScope scope; scope.skip(99); return; }
void issue() { NxboxDrawScope scope; scope.issued(); }
void forward() { NxboxDrawScope scope; scope.forwarded(); issue(); scope.skip(101); }
int main(int argc,char **argv) {
 assert(argc==2); reports["NXBOX_DRAW_DIAG"]=argv[1];
 skip(); issue(); forward();
 nxbox_draw_diag_count(NXBOX_DRAW_LINK_FAIL);
 nxbox_draw_diag_count(NXBOX_DRAW_DXIL_FAIL);
 reports["NXBOX_GL_DRAW_SKIP_TOTAL"]="3";
 reports["NXBOX_GL_SWIZZLE_TOTAL"]="4";
 reports["NXBOX_PRIMCONVERT_DRAW_SKIP_TOTAL"]="1";
 reports["NXBOX_VBUF_DRAW_SKIP_TOTAL"]="2";
 nxbox_draw_diag_frame();
 if(std::string(argv[1])!="1") {
  assert(reports.count("NXBOX_D3D12_DRAW_DIAG")==0);
  assert(reports.count("NXBOX_DRAW_SKIP_LAST")==0); return 0;
 }
 auto first=reports.at("NXBOX_D3D12_DRAW_DIAG");
 assert(first.find("frame=1 gl_skipped=3 aux_skipped=3 viewport_swizzle=4")!=std::string::npos);
 assert(first.find("calls=4 issued=2 skipped=1 forwarded=1 link_fail=1 dxil_fail=1")!=std::string::npos);
 assert(reports.at("NXBOX_DRAW_SKIP_LAST")=="d3d12_draw_vbo:99");
 nxbox_draw_diag_frame();
 auto second=reports.at("NXBOX_D3D12_DRAW_DIAG");
 assert(second.find("frame=2 gl_skipped=0 aux_skipped=0 viewport_swizzle=0")!=std::string::npos);
 assert(second.find("calls=0 issued=0 skipped=0 forwarded=0 link_fail=0 dxil_fail=0")!=std::string::npos);
 assert(second.find("logic=0 dual=0 clip=0 points=0 point_sprites=0 stencil=0")!=std::string::npos);
}
"""
        )
        for gate in ("1", "0", "", "10"):
            with self.subTest(gate=gate):
                self.compile_run(code, [gate])

    def test_final_chain_exit_coverage_link_and_frame_hooks(self):
        source = (self.driver / "d3d12_draw.cpp").read_text()
        draw = source.split("void\nd3d12_draw_vbo(", 1)[1].split("\nstatic ", 1)[0]
        self.assertEqual(
            draw.count("return;"),
            draw.count("{ nxbox_draw_scope.skip(__LINE__); return; }"),
        )
        self.assertEqual(draw.count("nxbox_draw_scope.forwarded();"), 2)
        self.assertGreater(draw.index("nxbox_draw_scope.issued();"), draw.index(".ExecuteIndirect"))
        self.assertLess(draw.index("NXBOX_DRAW_RESTART"), draw.index("util_primconvert_draw_vbo"))
        linker = (self.mesa / "src/mesa/state_tracker/st_glsl_to_nir.cpp").read_text()
        self.assertIn("LinkStatus == LINKING_FAILURE", linker)
        self.assertIn("nxbox_draw_diag_count(NXBOX_DRAW_LINK_FAIL)", linker)
        gl = (self.mesa / "src/mesa/main/draw.c").read_text()
        self.assertGreater(gl.count("nxbox_gl_draw_skip(__func__, __LINE__)"), 35)
        present = (
            self.mesa / "src/gallium/winsys/d3d12/wgl/d3d12_wgl_framebuffer_uwp.cpp"
        ).read_text()
        self.assertEqual(present.count("nxbox_draw_diag_frame();"), 1)
        frontend = (ROOT / "src/eden_uwp/mesa_window.cpp").read_text()
        self.assertIn('Diagnostic(std::string("D3D12_DRAW_DIAG ") + report)', frontend)
        self.assertIn("NXBOX_GL_LINK_FAIL_LAST", frontend)

    def test_generated_state_observer_compiles_and_counts(self):
        source = (self.driver / "d3d12_draw.cpp").read_text()
        observer = (
            "   if (nxbox_draw_diag_enabled()) {"
            + source.split("   if (nxbox_draw_diag_enabled()) {", 1)[1].split(
                "   if (!prim_supported", 1
            )[0]
        )
        header = (self.driver / "nxbox_draw_diag.h").read_text().replace("#pragma once", "")
        format_names = sorted(set(re.findall(r"PIPE_FORMAT_\w+", observer)))
        code = (
            r"""
#include <cassert>
#include <string>
#include <cstring>
std::string report;
unsigned long GetEnvironmentVariableA(const char *key,char *text,unsigned long) {
 if(std::string(key)=="NXBOX_DRAW_DIAG") {std::strcpy(text,"1");return 1;}
 text[0]=0;return 0;
}
void SetEnvironmentVariableA(const char *,const char *text) {report=text;}
"""
            + header
            + "enum pipe_format { "
            + ",".join(format_names)
            + " };\n"
            + r"""
constexpr unsigned MESA_PRIM_POINTS=0,PIPE_SHADER_GEOMETRY=3,PIPE_SHADER_FRAGMENT=4,
 PIPE_SHADER_TESS_CTRL=1,PIPE_SHADER_VERTEX=0,
 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA=1,FRAG_RESULT_DEPTH=2;
#define BITFIELD64_BIT(x) (1ull<<(x))
struct Element { unsigned InputSlot,InputSlotClass,InstanceDataStepRate; };
struct d3d12_vertex_elements_state { unsigned num_elements=3; Element elements[3]{{0,1,2},{0,0,0},{1,1,1}};
 pipe_format format_conversion[3]{PIPE_FORMAT_R10G10B10A2_SNORM,PIPE_FORMAT_R10G10B10A2_SNORM,
                                PIPE_FORMAT_R16G16B16A16_USCALED}; };
struct Shader { struct { unsigned clip_distance_array_size=4; uint64_t outputs_written=4; } info; };
struct Selector { Shader *initial; };
struct Screen { struct { bool IndependentFrontAndBackStencilRefMaskSupported=false; } opts14; };
Screen *d3d12_screen(Screen *screen) { return screen; }
struct Blend {struct {struct {bool LogicOpEnable=true;} RenderTarget[8];
 bool IndependentBlendEnable=true,AlphaToCoverageEnable=true;} desc;
 bool is_dual_src=true; unsigned blend_factor_flags=1;};
struct Raster {struct {unsigned clip_plane_enable=3,sprite_coord_enable=1;
 bool scissor=true,rasterizer_discard=true,depth_clip_near=true,depth_clip_far=false;} base;struct {bool DepthClipEnable=false;} desc;};
struct Depth {struct {bool StencilEnable=true,DepthBoundsTestEnable=true;} desc; bool backface_enabled=true,nxbox_stencil_mask_mismatch=true;};
struct Viewport {float TopLeftX=-1,TopLeftY=0,Width=1920,Height=1080;};
struct Context {struct {Blend *blend;Raster *rast;Depth *zsa;
 unsigned num_so_targets=1,sample_mask=0;
 d3d12_vertex_elements_state *ves=nullptr; unsigned num_cbufs=2;} gfx_pipeline_state;
 Selector *gfx_stages[5]{}; void *current_predication=nullptr,*cmdlist8=nullptr;
 struct { Screen *screen; } base;
 struct { unsigned ref_value[2]{1,2}; } stencil_ref;
 unsigned num_viewports=1;Viewport viewports[1];struct {unsigned width=1280,height=720;} fb;};
struct Info {unsigned mode=MESA_PRIM_POINTS,instance_count=2,start_instance=4;
 bool primitive_restart=true;};
void observe(Context *ctx,Info *dinfo) {
"""
            + observer
            + r"""
}
int main() {
 Blend blend; Raster raster; Depth depth; Context ctx{}; Info info;
 ctx.gfx_pipeline_state={&blend,&raster,&depth,1,0};
 Screen screen; ctx.base.screen=&screen;
 Shader shader; Selector selector{&shader}; d3d12_vertex_elements_state ves;
 ctx.current_predication=&ctx;ctx.gfx_stages[3]=&selector;ctx.gfx_stages[4]=&selector;
 ctx.gfx_pipeline_state.ves=&ves;
 observe(&ctx,&info);nxbox_draw_diag_frame();
 for(const char *field: {"logic=1","dual=1","clip=1","point_sprites=1","stencil=1",
 "restart=1","gs=1","so=1","constant=1","independent=1","a2c=1","sample_mask=1",
 "depth_bounds=1","depth_clamp=1","viewport=1","scissor=1","predication=1","discard=1",
 "instance_attr=1","divisor_gt1=1","base_instance=1","instance_slot_conflict=1",
 "clip_written=1","clip_disabled_written=1","depth_clip_asymmetric=1","frag_depth=1",
 "stencil_ref_split=1","stencil_ref_unsupported=1","independent_mrt=1",
 "stencil_mask_split=1","stencil_mask_unsupported=1",
 "fmt_r10g10b10a2_snorm=1","fmt_r16g16b16a16_uscaled=1","fmt_r8g8b8_sint=0"})
  assert(report.find(field)!=std::string::npos);
 ctx.gfx_pipeline_state={nullptr,nullptr,nullptr,0,~0u};
 observe(&ctx,&info); // Empty state must be safe to inspect.
}
"""
        )
        self.compile_run(code)

    def test_requested_stencil_masks_survive_capability_fallback(self):
        context = (self.driver / "d3d12_context.cpp").read_text()
        assignment = (
            "dsa->nxbox_stencil_mask_mismatch ="
            + context.split("dsa->nxbox_stencil_mask_mismatch =", 1)[1].split(";", 1)[0]
            + ";"
        )
        self.compile_run(
            r"""
#include <cassert>
#include <initializer_list>
struct Stencil { bool enabled; unsigned valuemask,writemask; };
struct State { Stencil stencil[2]; };
struct Dsa { bool nxbox_stencil_mask_mismatch; };
void capture(Dsa *dsa,const State *depth_stencil_alpha) {
"""
            + assignment
            + r"""
}
int main() {
 Dsa dsa{};
 for(unsigned front=0;front<256;++front) for(unsigned back=0;back<256;++back)
 for(unsigned enabled=0;enabled<4;++enabled) for(bool split_write:{false,true}) {
  State state{{{bool(enabled&1),front,255},{bool(enabled&2),back,split_write?0u:255u}}};
  capture(&dsa,&state);
  assert(dsa.nxbox_stencil_mask_mismatch==
         (enabled==3 && (front!=back || split_write)));
 }
}
"""
        )

    def test_counter_storage_is_shared_between_translation_units(self):
        header = (self.driver / "nxbox_draw_diag.h").read_text()
        (self.root / "diag.h").write_text(
            "unsigned long GetEnvironmentVariableA(const char*,char*,unsigned long);\n"
            "void SetEnvironmentVariableA(const char*,const char*);\n" + header
        )
        (self.root / "producer.cpp").write_text(
            '#include "diag.h"\nvoid producer() {nxbox_draw_diag_count(NXBOX_DRAW_LINK_FAIL);}\n'
        )
        (self.root / "consumer.cpp").write_text(r"""
#include "diag.h"
#include <cassert>
#include <cstring>
#include <string>
std::string report;
unsigned long GetEnvironmentVariableA(const char *key,char *text,unsigned long) {
 if(std::string(key)=="NXBOX_DRAW_DIAG") {std::strcpy(text,"1");return 1;}
 text[0]=0;return 0;
}
void SetEnvironmentVariableA(const char *,const char *text) {report=text;}
void producer();
int main() {producer();nxbox_draw_diag_frame();assert(report.find("link_fail=1")!=std::string::npos);}
""")
        binary = self.root / "shared"
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                str(self.root / "producer.cpp"),
                str(self.root / "consumer.cpp"),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
