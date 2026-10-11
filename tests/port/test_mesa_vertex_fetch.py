# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute patched vertex conversions and cache comparisons on the host."""

import inspect
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("NXBOX_MESA_SRC", "/tmp/mesa-pin"))
DRIVER = Path("src/gallium/drivers/d3d12")
CONVERSION = Path("src/microsoft/compiler/dxil_nir_lower_vs_vertex_conversion.c")
sys.path.insert(0, str(ROOT / "tools/nxbox"))
import patch_mesa_uwp as patcher


class VertexFetchTests(unittest.TestCase):
    def test_patch_is_called(self):
        source = inspect.getsource(patcher.patch)
        self.assertEqual(source.count("patch_vertex_fetch(root)"), 1)
        self.assertLess(
            source.index("patch_vertex_fetch(root)"), source.index("patch_geometry_diag(root)")
        )

    @unittest.skipUnless(SOURCE.is_dir(), "Provide a pristine pinned Mesa checkout")
    def test_generated_alignment_conversion_and_keys(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-vertex-") as temporary:
            root = Path(temporary)
            for file in (DRIVER / "d3d12_screen.cpp", DRIVER / "d3d12_compiler.cpp", CONVERSION):
                target = root / file
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(SOURCE / file, target)
            patcher.patch_vertex_fetch(root)
            screen = (root / DRIVER / "d3d12_screen.cpp").read_text()
            caps = screen.split("   case PIPE_CAP_PRIMITIVE_RESTART:", 1)[1].split(
                "      return 1;", 1
            )[0]
            for cap in ("OFFSET", "STRIDE"):
                self.assertIn(f"PIPE_CAP_VERTEX_BUFFER_{cap}_4BYTE_ALIGNED_ONLY", caps)
            element_caps = screen.split("   case PIPE_CAP_SEAMLESS_CUBE_MAP:", 1)[1].split(
                "      return 1;", 1
            )[0]
            self.assertIn("PIPE_CAP_VERTEX_ELEMENT_SRC_OFFSET_4BYTE_ALIGNED_ONLY", element_caps)
            self.assertNotIn("PIPE_CAP_VERTEX_ATTRIB_ELEMENT_ALIGNED_ONLY", caps)
            # Compile the actual case labels as well, catching duplicate-cap additions.
            cap_names = re.findall(r"case (PIPE_CAP_\w+):", screen)
            # The conservative four-byte caps feed the existing translation path.
            vbuf = (SOURCE / "src/gallium/auxiliary/util/u_vbuf.c").read_text()
            self.assertIn("!mgr->caps.buffer_offset_unaligned && vb->buffer_offset % 4 != 0", vbuf)
            self.assertIn("!mgr->caps.velem_src_offset_unaligned &&", vbuf)
            self.assertIn("ve->ve[i].src_offset % 4 != 0", vbuf)

            conversion = (root / CONVERSION).read_text()
            helpers = (
                "typedef  nir_def *"
                + conversion.split("typedef  nir_def *", 1)[1].split(
                    "static nir_def *\nlower_vs_vertex_conversion_impl", 1
                )[0]
            )
            # Execute the real dispatch as well as its packed helpers.
            format_names = re.findall(r"PIPE_FORMAT_\w+", conversion)
            formats = sorted(set(format_names))
            dispatch = (
                "static nir_def *\nlower_vs_vertex_conversion_impl"
                + conversion.split("static nir_def *\nlower_vs_vertex_conversion_impl", 1)[1].split(
                    "/* Lower emulated vertex attribute input", 1
                )[0]
            )
            helpers += (
                "\nenum pipe_format { "
                + ",".join(formats)
                + " };\n"
                + r"""
struct nir_variable { pipe_format format; };
struct nir_intrinsic_instr { nir_def def; nir_variable *var; };
using nir_instr=nir_intrinsic_instr;
nir_intrinsic_instr *nir_instr_as_intrinsic(nir_instr *i) { return i; }
nir_variable *nir_intrinsic_get_var(nir_intrinsic_instr *i,unsigned) { return i->var; }
pipe_format get_input_target_format(nir_variable *v,void *) { return v->format; }
bool util_format_has_alpha(pipe_format f) {
 return f!=PIPE_FORMAT_R8G8B8_SINT && f!=PIPE_FORMAT_R8G8B8_UINT &&
        f!=PIPE_FORMAT_R16G16B16_SINT && f!=PIPE_FORMAT_R16G16B16_UINT;
}
nir_def *nir_channel(nir_builder *,nir_def *a,unsigned channel) {
 auto *n=node(); n->u.fill(a->u[channel]); return n;
}
nir_def *nir_imm_int(nir_builder *b,int value) { return nir_imm_ivec4(b,value,value,value,value); }
nir_def *nir_vector_insert_imm(nir_builder *,nir_def *a,nir_def *v,unsigned lane) {
 auto *n=node(); *n=*a; n->u[lane]=v->u[0]; return n;
}
nir_def *nir_u2f32(nir_builder *,nir_def *a) {
 auto *n=node(); for(unsigned i=0;i<4;++i)n->f[i]=float(a->u[i]); return n;
}
#define unreachable(message) abort()
"""
                + dispatch
            )
            compiler = (root / DRIVER / "d3d12_compiler.cpp").read_text()
            compare = compiler.split("switch (expect->stage)", 1)[1].split(
                "case PIPE_SHADER_GEOMETRY:", 1
            )[0]
            compare = compare.split("case PIPE_SHADER_VERTEX:", 1)[1].rsplit("break;", 1)[0]
            self.assertIn("sizeof(expect->vs.format_conversion)", compare)
            fill = compiler.split("   if (stage == PIPE_SHADER_VERTEX &&", 1)[1].split(
                "   if (stage == PIPE_SHADER_FRAGMENT", 1
            )[0]
            self.assertIn("memset(key->vs.format_conversion + num_elements", fill)
            self.assertIn("memcpy(key->vs.format_conversion", fill)

            runner = safety.MesaRenderSafetyTests()
            runner.root = root
            runner.compile_run(
                "enum Caps { "
                + ",".join(sorted(set(cap_names)))
                + " };\nint main() { switch (PIPE_CAP_VERTEX_BUFFER_OFFSET_4BYTE_ALIGNED_ONLY) {\n"
                + "\n".join(f"case {name}: break;" for name in cap_names)
                + "} }\n"
            )
            runner.compile_run(
                r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
struct nir_builder {};
struct nir_def { std::array<uint32_t,4> u{}; std::array<float,4> f{}; unsigned num_components=4; };
std::deque<nir_def> nodes;
nir_def *node() { nodes.emplace_back(); return &nodes.back(); }
nir_def *nir_imm_ivec4(nir_builder *,uint32_t x,uint32_t y,uint32_t z,uint32_t w) {
 auto *n=node(); n->u={x,y,z,w}; return n;
}
nir_def *nir_imm_vec4(nir_builder *,float x,float y,float z,float w) {
 auto *n=node(); n->f={x,y,z,w}; return n;
}
nir_def *nir_imm_float(nir_builder *b,float f) { return nir_imm_vec4(b,f,f,f,f); }
nir_def *nir_ishl(nir_builder *,nir_def *a,nir_def *b) {
 auto *n=node(); for(int i=0;i<4;++i) n->u[i]=a->u[i]<<b->u[i]; return n;
}
nir_def *nir_ushr(nir_builder *,nir_def *a,nir_def *b) {
 auto *n=node(); for(int i=0;i<4;++i) n->u[i]=a->u[i]>>b->u[i]; return n;
}
nir_def *nir_ishr(nir_builder *,nir_def *a,nir_def *b) {
 auto *n=node(); for(int i=0;i<4;++i) n->u[i]=int32_t(a->u[i])>>b->u[i]; return n;
}
nir_def *nir_i2f32(nir_builder *,nir_def *a) {
 auto *n=node(); for(int i=0;i<4;++i) n->f[i]=float(int32_t(a->u[i])); return n;
}
nir_def *nir_fmul(nir_builder *,nir_def *a,nir_def *b) {
 auto *n=node(); for(int i=0;i<4;++i) n->f[i]=a->f[i]*b->f[i]; return n;
}
nir_def *nir_fmax(nir_builder *,nir_def *a,nir_def *b) {
 auto *n=node(); for(int i=0;i<4;++i) n->f[i]=std::max(a->f[i],b->f[i]); return n;
}
"""
                + helpers
                + r"""
struct Key { struct { bool needs_format_emulation=true; int format_conversion[32]{}; } vs; };
bool same(const Key *expect,const Key *have) {
"""
                + compare
                + r"""
 return true;
}
int main() {
 nir_builder b;
 // Exhaust every signed/unsigned code in every packed lane, including BGRA.
 for(bool bgra:{false,true}) for(unsigned lane=0;lane<4;++lane) {
  unsigned bits=lane==3 ? 2 : 10;
  unsigned shift=lane==3 ? 30 : (bgra ? 2-lane : lane)*10;
  unsigned limit=1u<<bits;
  for(unsigned code=0;code<limit;++code) {
   nodes.clear(); nir_def src; src.u.fill(code<<shift);
   auto *ls=bgra ? lshift_bgra(&b) : lshift_rgba(&b);
   int signed_code=int(code)-(code & (limit/2) ? int(limit) : 0);
   auto *snorm=from_10_10_10_2_snorm(&b,&src,ls);
   float expected=std::max(-1.0f,float(signed_code)/float(limit/2-1));
   assert(std::abs(snorm->f[lane]-expected)<0.000001f);
   auto *sscaled=from_10_10_10_2_scaled(&b,&src,ls,nir_ishr);
   assert(sscaled->f[lane]==float(signed_code));
   auto *uscaled=from_10_10_10_2_scaled(&b,&src,ls,nir_ushr);
   assert(uscaled->f[lane]==float(code));
   auto *unorm=from_10_10_10_2_unorm(&b,&src,ls);
   assert(std::abs(unorm->f[lane]-float(code)/float(limit-1))<0.000001f);
  }
 }
 // Scaled formats convert integers numerically; neither signed nor unsigned clamps.
 for(unsigned bits:{8u,16u}) for(bool sign:{false,true}) {
  nir_variable var{bits==8 ? (sign?PIPE_FORMAT_R8G8B8A8_SSCALED:PIPE_FORMAT_R8G8B8A8_USCALED)
                          : (sign?PIPE_FORMAT_R16G16B16A16_SSCALED:PIPE_FORMAT_R16G16B16A16_USCALED)};
  for(unsigned code=0;code<(1u<<bits);++code) {
   nodes.clear(); int value=int(code)-(sign && (code&(1u<<(bits-1))) ? int(1u<<bits):0);
   nir_intrinsic_instr instr; instr.var=&var; instr.def.u.fill(value);
   auto *result=lower_vs_vertex_conversion_impl(&b,&instr,nullptr);
   for(unsigned lane=0;lane<4;++lane) assert(result->f[lane]==float(value));
  }
 }
 // RGB integer IA widening preserves RGB and replaces the unrelated fourth lane with integer 1.
 for(auto format:{PIPE_FORMAT_R8G8B8_SINT,PIPE_FORMAT_R8G8B8_UINT,
                  PIPE_FORMAT_R16G16B16_SINT,PIPE_FORMAT_R16G16B16_UINT}) {
  nodes.clear(); nir_variable var{format}; nir_intrinsic_instr instr; instr.var=&var;
  instr.def.u={uint32_t(-32768),65535,127,0xdeadbeef};
  auto *result=lower_vs_vertex_conversion_impl(&b,&instr,nullptr);
  assert(result->u[0]==instr.def.u[0] && result->u[1]==65535 && result->u[2]==127);
  assert(result->u[3]==1);
  instr.def.num_components=3; assert(!lower_vs_vertex_conversion_impl(&b,&instr,nullptr));
 }
 Key a,bkey; a.vs.format_conversion[0]=bkey.vs.format_conversion[0]=1;
 assert(same(&a,&bkey));
 // Same hash and same prefix, but a removed attribute needs no conversion now.
 bkey.vs.format_conversion[7]=2; assert(!same(&a,&bkey));
 bkey=a; bkey.vs.format_conversion[0]=3; assert(!same(&a,&bkey));
 bkey=a; bkey.vs.needs_format_emulation=false; assert(!same(&a,&bkey));
 a.vs.needs_format_emulation=false; assert(same(&a,&bkey));
}
"""
            )

    @unittest.skipUnless(SOURCE.is_dir(), "Provide a pristine pinned Mesa checkout")
    def test_emulated_inventory_and_half_float_fallback(self):
        formats = (SOURCE / DRIVER / "d3d12_format.c").read_text()
        emulation = formats.split("d3d12_emulated_vtx_format(enum pipe_format fmt)", 1)[1].split(
            "unsigned\nd3d12_non_opaque_plane_count", 1
        )[0]
        inventory = set(re.findall(r"case (PIPE_FORMAT_\w+):", emulation))
        expected = {
            "PIPE_FORMAT_R10G10B10A2_SNORM",
            "PIPE_FORMAT_R10G10B10A2_SSCALED",
            "PIPE_FORMAT_R10G10B10A2_USCALED",
            "PIPE_FORMAT_B10G10R10A2_UNORM",
            "PIPE_FORMAT_B10G10R10A2_SNORM",
            "PIPE_FORMAT_B10G10R10A2_SSCALED",
            "PIPE_FORMAT_B10G10R10A2_USCALED",
            "PIPE_FORMAT_R8G8B8_SINT",
            "PIPE_FORMAT_R8G8B8_UINT",
            "PIPE_FORMAT_R16G16B16_SINT",
            "PIPE_FORMAT_R16G16B16_UINT",
            "PIPE_FORMAT_R8G8B8A8_SSCALED",
            "PIPE_FORMAT_R8G8B8A8_USCALED",
            "PIPE_FORMAT_R16G16B16A16_SSCALED",
            "PIPE_FORMAT_R16G16B16A16_USCALED",
        }
        self.assertEqual(inventory, expected)
        diagnostic = (ROOT / "tools/nxbox/patch_mesa_uwp.py").read_text()
        for name in expected:
            self.assertIn("{" + name + ", NXBOX_DRAW_FMT_", diagnostic)
        # Half and BGRA8 use native IA support when available; otherwise u_vbuf translates.
        for channels in ("R16", "R16G16", "R16G16B16A16"):
            self.assertIn(f"MAP_FORMAT_FLOAT({channels})", formats)
        self.assertIn("MAP_FORMAT(B8G8R8A8, UNORM)", formats)
        vbuf = (SOURCE / "src/gallium/auxiliary/util/u_vbuf.c").read_text()
        self.assertIn("{ PIPE_FORMAT_R16G16B16_FLOAT,      PIPE_FORMAT_R32G32B32_FLOAT }", vbuf)
        half = (SOURCE / "src/util/half_float.c").read_text()
        half = (
            "float\n_mesa_half_to_float_slow"
            + half.split("float\n_mesa_half_to_float_slow", 1)[1].split("/**", 1)[0]
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-half-") as temporary:
            runner = safety.MesaRenderSafetyTests()
            runner.root = Path(temporary)
            runner.compile_run(
                "#include <cstdint>\n#include <cmath>\n#include <cassert>\n"
                "union fi { uint32_t ui; float f; };\n"
                + half
                + r"""
int main() {
 for(unsigned code=0;code<65536;++code) {
  unsigned exponent=(code>>10)&31,mantissa=code&1023;
  float actual=_mesa_half_to_float_slow(code);
  if(exponent==31) {
   assert(mantissa?std::isnan(actual):std::isinf(actual));
  } else {
   float expected=exponent?std::ldexp(1.0f+float(mantissa)/1024,int(exponent)-15)
                          :std::ldexp(float(mantissa),-24);
   if(code&0x8000)expected=-expected;
   assert(actual==expected);
   assert(std::signbit(actual)==bool(code&0x8000));
  }
 }
}
"""
            )


if __name__ == "__main__":
    unittest.main()
