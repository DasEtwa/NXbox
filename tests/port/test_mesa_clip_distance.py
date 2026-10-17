# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute Mesa's disabled clip-distance stores and verify variant integration."""

import inspect
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("NXBOX_MESA_SRC", "/tmp/mesa-pin"))
DRIVER = Path("src/gallium/drivers/d3d12")
sys.path.insert(0, str(ROOT / "tools/nxbox"))
import patch_mesa_uwp as patcher


class ClipDistanceTests(unittest.TestCase):
    def test_patch_is_called(self):
        source = inspect.getsource(patcher.patch)
        self.assertEqual(source.count("patch_clip_distance(root)"), 1)
        self.assertLess(
            source.index("patch_clip_distance(root)"), source.index("patch_draw_diag(root)")
        )

    @unittest.skipUnless(SOURCE.is_dir(), "Provide a pristine pinned Mesa checkout")
    def test_mask_is_keyed_and_only_final_stage_is_lowered(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-clip-") as temporary:
            root = Path(temporary)
            for name in ("d3d12_compiler.cpp", "d3d12_compiler.h"):
                target = root / DRIVER / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(SOURCE / DRIVER / name, target)
            patcher.patch_clip_distance(root)
            header = (root / DRIVER / "d3d12_compiler.h").read_text()
            source = (root / DRIVER / "d3d12_compiler.cpp").read_text()
            common = header.split("   union {", 1)[1].split("   unsigned tex_saturate_s", 1)[0]
            key = "struct Key { union {" + common + "};"
            runner = safety.MesaRenderSafetyTests()
            runner.root = root
            runner.compile_run(
                "#include <cstdint>\n#include <cassert>\n"
                + key
                + r"""
int main() {
 static_assert(sizeof(Key)==sizeof(uint32_t));
 Key a{},b{};
 a.last_vertex_processing_stage=b.last_vertex_processing_stage=1;
 for(unsigned mask=0;mask<256;++mask) {
  a.clip_plane_enable=mask;
  for(unsigned other=0;other<256;++other) {
   b.clip_plane_enable=other;
   assert((a.common_all==b.common_all)==(mask==other));
  }
  assert(a.last_vertex_processing_stage==1);
 }
}
"""
            )
            self.assertIn("expect->common_all == have->common_all", source)
            self.assertIn("hash += key->common_all;", source)
            fill = source.split("      key->last_vertex_processing_stage = 1;", 1)[1]
            self.assertIn("rast->base.clip_plane_enable : 0;", fill)
            lowering = source.split("   if (key->last_vertex_processing_stage) {", 1)[1].split(
                "   NIR_PASS_V(nir, d3d12_lower_load_draw_params)", 1
            )[0]
            self.assertIn(
                "NIR_PASS_V(nir, nir_lower_clip_disable, key->clip_plane_enable);", lowering
            )
            self.assertLess(
                lowering.index("nir_lower_clip_disable"), lowering.index("nir_lower_clip_halfz")
            )
            self.assertEqual(source.count("nir_lower_clip_disable"), 1)

    @unittest.skipUnless(SOURCE.is_dir(), "Provide a pristine pinned Mesa checkout")
    def test_real_nir_store_lowering_all_masks(self):
        # DXIL splits clip distances into CLIP_DIST0/1 vectors before variant compilation.
        lowering = (SOURCE / "src/compiler/nir/nir_lower_clip_disable.c").read_text()
        lowering = (
            "static void\nrecursive_if_chain"
            + lowering.split("static void\nrecursive_if_chain", 1)[1].split(
                "bool\nnir_lower_clip_disable", 1
            )[0]
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-clip-nir-") as temporary:
            runner = safety.MesaRenderSafetyTests()
            runner.root = Path(temporary)
            runner.compile_run(
                r"""
#include <array>
#include <deque>
#include <cassert>
#include <cstdint>
enum { VARYING_SLOT_CLIP_DIST0, VARYING_SLOT_CLIP_DIST1, OTHER,
 nir_var_shader_out=5,nir_var_shader_in,nir_intrinsic_store_deref,
 nir_deref_type_var,nir_deref_type_array };
struct nir_def { std::array<uint32_t,4> value{}; bool constant=true; };
struct nir_variable { struct { unsigned location,mode; } data; };
struct nir_src { nir_def *ssa; };
struct nir_deref_instr {
 unsigned deref_type=nir_deref_type_var; nir_variable *var;
 struct { nir_src index; } arr; void *type=nullptr;
};
struct nir_intrinsic_instr {
 unsigned intrinsic=nir_intrinsic_store_deref;
 nir_src src[2]; unsigned num_components=4,mask=15; int instr=0;
};
struct nir_builder { int cursor=0; std::array<uint32_t,4> stored{}; unsigned stores=0; };
std::deque<nir_def> nodes;
nir_def *node() { nodes.emplace_back(); return &nodes.back(); }
nir_def *nir_imm_int(nir_builder *,unsigned x) { auto *n=node(); n->value.fill(x); return n; }
nir_def *nir_channel(nir_builder *b,nir_def *n,unsigned i) { return nir_imm_int(b,n->value[i]); }
nir_def *nir_undef(nir_builder *b,unsigned,unsigned) { return nir_imm_int(b,0xdeadbeef); }
nir_def *nir_vec(nir_builder *,nir_def **v,unsigned count) {
 auto *n=node(); for(unsigned i=0;i<count;++i)n->value[i]=v[i]->value[0]; return n;
}
nir_deref_instr *nir_src_as_deref(nir_src s) { return reinterpret_cast<nir_deref_instr *>(s.ssa); }
nir_variable *nir_deref_instr_get_variable(nir_deref_instr *d) { return d->var; }
unsigned nir_intrinsic_write_mask(nir_intrinsic_instr *i) { return i->mask; }
int nir_after_instr(int *) { return 1; }
bool nir_src_is_const(nir_src s) { return s.ssa->constant; }
unsigned nir_src_as_uint(nir_src s) { return s.ssa->value[0]; }
void nir_store_deref(nir_builder *b,nir_deref_instr *,nir_def *n,unsigned mask) {
 ++b->stores; for(unsigned i=0;i<4;++i)if(mask&(1u<<i))b->stored[i]=n->value[i];
}
void nir_instr_remove(int *) {}
nir_deref_instr *nir_deref_instr_parent(nir_deref_instr *d) { return d; }
unsigned glsl_get_length(void *) { return 8; }
nir_def *nir_ilt_imm(nir_builder *,nir_def *n,unsigned) { return n; }
void nir_push_if(nir_builder *,nir_def *) {}
void nir_push_else(nir_builder *,void *) {}
void nir_pop_if(nir_builder *,void *) {}
"""
                + lowering
                + r"""
int main() {
 for(unsigned enable=0;enable<256;++enable) for(unsigned location=0;location<2;++location)
 for(unsigned write=1;write<16;++write) {
  nodes.clear(); nir_builder b;
  nir_variable var{{location,nir_var_shader_out}};
  nir_deref_instr deref; deref.var=&var;
  nir_def value; value.value={0xbf800000,0x3f800000,0xc0000000,0x7fc00000};
  nir_intrinsic_instr instr; instr.src[0].ssa=reinterpret_cast<nir_def *>(&deref);
  instr.src[1].ssa=&value; instr.mask=write;
  assert(lower_clip_plane_store(&b,&instr,&enable)); assert(b.stores==1);
  for(unsigned lane=0;lane<4;++lane) if(write&(1u<<lane))
   assert(b.stored[lane]==((enable&(1u<<(location*4+lane)))?value.value[lane]:0));
  // Unrelated outputs and inputs must not be changed.
  var.data.location=OTHER; assert(!lower_clip_plane_store(&b,&instr,&enable));
  var.data.location=location; var.data.mode=nir_var_shader_in;
  assert(!lower_clip_plane_store(&b,&instr,&enable));
 }
}
"""
            )


if __name__ == "__main__":
    unittest.main()
