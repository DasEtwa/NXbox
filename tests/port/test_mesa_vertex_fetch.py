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
#include <deque>
struct nir_builder {};
struct nir_def { std::array<uint32_t,4> u{}; std::array<float,4> f{}; };
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


if __name__ == "__main__":
    unittest.main()
