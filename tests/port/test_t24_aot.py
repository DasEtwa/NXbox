# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and execute the AOT discovery and persistent-store contracts."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class T24AotTests(unittest.TestCase):
    def run_cpp(self, body, runs=((),)):
        with tempfile.TemporaryDirectory(prefix="nxbox-t24-") as directory:
            source = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            source.write_text(
                '#include <cassert>\n#include "core/arm/aot/image.h"\n'
                '#include "core/arm/aot/precompile.h"\n'
                '#include "common/nxbox_jit_cache.h"\n'
                "using namespace Core::Aot;\n"
                "void Put(std::vector<std::uint8_t>& b, size_t at, uint64_t v, size_t n) {\n"
                "assert(at + n <= b.size());\n"
                "for(size_t i=0;i<n;++i) b[at+i]=static_cast<uint8_t>(v>>(8*i)); }\n" + body
            )
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'src'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            for args in runs:
                subprocess.run([str(binary), directory, *args], check=True, timeout=30)

    def test_reachable_branches_prologues_and_relocated_pointers(self):
        self.run_cpp(
            r"""
int main() {
    Module m{{{0x1000, std::vector<uint8_t>(0x80), true},
              {0x2000, std::vector<uint8_t>(16), false}}, {0x1000}, {1}};
    auto& b=m.ranges[0].bytes;
    Put(b,0,0x94000004,4); // BL 1010, return 1004
    Put(b,4,0x54000060,4); // B.eq 1010, fallthrough 1008
    Put(b,8,0x340000c0,4); // CBZ 1020, fallthrough 100c
    Put(b,12,0xd65f03c0,4);
    Put(b,16,0x36000080,4); // TBZ 1020, fallthrough 1014
    Put(b,20,0x17fffffe,4); // backward B 100c
    Put(b,32,0xd63f0000,4); // BLR, continuation 1024
    Put(b,36,0xd65f03c0,4);
    Put(b,48,0xa9bf7bfd,4); // independent function prologue
    Put(b,52,0xd65f03c0,4);
    Put(b,64,0xd61f0000,4); // indirect PLT target via relocation
    Put(m.ranges[1].bytes,0,0x1040,8);
    Put(m.ranges[1].bytes,8,0xffffffffffffffffULL,8);
    const auto d=Discover(m);
    const std::set<Address> expected{0x1000,0x1004,0x1008,0x100c,0x1010,
                                     0x1014,0x1020,0x1024,0x1030,0x1040};
    assert(d.blocks==expected && !d.truncated && d.unresolved_indirect==2);
    assert(d.instructions==11);
    auto capped=Discover(m, {3,3});
    assert(capped.truncated && capped.blocks.size()<=3 && capped.instructions<=3);
    assert(!Relative(0,0x03ffffff,26));
    assert(!Relative(UINT64_MAX,1,26));
    assert(!Instruction(m,0x1001) && !Instruction(m,0x2000));
}
"""
        )

    def test_elf_symbols_init_array_and_malformed_headers(self):
        self.run_cpp(
            r"""
int main() {
    std::vector<uint8_t> f(0x300);
    Put(f,0,0x464c457f,4); f[4]=2; f[5]=1;
    Put(f,18,183,2); Put(f,24,0x1000,8);
    Put(f,32,64,8); Put(f,54,56,2); Put(f,56,1,2);
    Put(f,64,1,4); Put(f,68,5,4); Put(f,72,0x100,8);
    Put(f,80,0x1000,8); Put(f,96,0x40,8);
    Put(f,40,0x180,8); Put(f,58,64,2); Put(f,60,2,2);
    Put(f,0x184,11,4); Put(f,0x198,0x240,8); Put(f,0x1a0,24,8);
    Put(f,0x1b8,24,8); f[0x244]=2; Put(f,0x246,1,2); Put(f,0x248,0x1010,8);
    Put(f,0x1c4,14,4); Put(f,0x1d8,0x280,8); Put(f,0x1e0,8,8);
    Put(f,0x280,0x1020,8);
    for(auto at: {0x100,0x110,0x120}) Put(f,at,0xd65f03c0,4);
    const auto m=ParseImage(f,0x4000);
    assert(m && Discover(*m).blocks==std::set<Address>({0x5000,0x5010,0x5020}));
    for(size_t size=0;size<64;++size) assert(!ParseImage(std::span(f).first(size)));
    auto bad=f; bad[5]=2; assert(!ParseImage(bad));
    bad=f; Put(bad,32,UINT64_MAX,8); assert(!ParseImage(bad));
    bad=f; Put(bad,96,UINT64_MAX,8); assert(!ParseImage(bad));
    bad=f; Put(bad,0x198,UINT64_MAX,8); assert(!ParseImage(bad));
    assert(!ParseImage(f,UINT64_MAX));
}
"""
        )

    def test_nso_nro_and_synthetic_second_process_cache_reuse(self):
        self.run_cpp(
            r"""
int main(int argc, char** argv) {
    assert(argc==3);
    std::vector<uint8_t> f(0x160);
    Put(f,0,0x304f534e,4); f[0x40]=42;
    Put(f,0x10,0x100,4); Put(f,0x14,0,4); Put(f,0x18,16,4);
    Put(f,0x20,0x110,4); Put(f,0x24,0x1000,4); Put(f,0x28,24,4);
    Put(f,0x30,0x140,4); Put(f,0x34,0x2000,4); Put(f,0x38,8,4);
    Put(f,0x8c,24,4); f[0x114]=2; Put(f,0x116,1,2); Put(f,0x118,8,8);
    Put(f,0x100,0x14000002,4); Put(f,0x108,0xd65f03c0,4);
    const auto m=ParseImage(f,0x8000); assert(m && m->build_id[0]==42);
    const auto d=Discover(*m); assert(d.blocks==std::set<Address>({0x8000,0x8008}));
    NxboxJitCache::Store store(std::filesystem::path(argv[1])/"aot.bin",1,{});
    const NxboxJitCache::Bytes config{1,2,3};
    assert(store.RegisterConfig(config)); store.Open(m->build_id);
    const auto stats=Precompile(d,[&](Address pc) {
        if(auto saved=store.Find(pc,config)) {
            assert(NxboxJitCache::MatchesGuest(*saved,[&](Address at){return Instruction(*m,at);}));
            return CompileResult::Restored;
        }
        // Synthetic translator exercises orchestration/storage, not x86 generation.
        NxboxJitCache::Record r;
        r.pc=r.descriptor=pc; r.config=config;
        auto word=Instruction(*m,pc); assert(word);
        r.guest.resize(4); Put(r.guest,0,*word,4);
        r.host={0x90}; r.relocation.offset=1;
        store.Insert(r); return CompileResult::Compiled;
    });
    if(std::string(argv[2])=="first") assert(stats.compiled==2 && stats.restored==0);
    else assert(stats.compiled==0 && stats.restored==2);
    store.Flush();
    assert(std::filesystem::file_size(std::filesystem::path(argv[1])/"aot.bin")<1024);
    auto bad=f; Put(bad,12,1,4); assert(!ParseImage(bad));
    bad=f; Put(bad,0x8c,25,4); assert(!ParseImage(bad));
    store.Open({43}); assert(!store.Find(0x8000,config));
    assert(!store.Find(0x8000,{4}));
    std::vector<uint8_t> nro(0x90);
    Put(nro,0x10,0x304f524e,4); Put(nro,0x18,nro.size(),4);
    Put(nro,0x20,0x80,4); Put(nro,0x24,4,4); Put(nro,0x80,0xd65f03c0,4);
    auto n=ParseImage(nro,0x8000); assert(n && Instruction(*n,0x8080));
}
""",
            runs=(("first",), ("second",)),
        )

    def test_backend_hook_is_optional_and_does_not_execute_guest(self):
        self.run_cpp(
            r"""
struct CurrentJit {};
struct FutureJit { int PrecompileBlock(Address pc) { assert(pc==0x1000); return 1; } };
int main() {
    CurrentJit current; FutureJit future;
    assert(CompileBlock(current,0x1000)==CompileResult::Unsupported);
    assert(CompileBlock(future,0x1000)==CompileResult::Compiled);
    Discovery d; d.blocks={0x1000};
    auto stats=Precompile(d,[&](auto pc) { return CompileBlock(current,pc); });
    assert(stats.discovered==1 && stats.unsupported==1 && stats.compiled==0);
    stats.RuntimeCompile(0x1000,d); stats.RuntimeCompile(0x2000,d);
    assert(stats.runtime_compiled==2 && stats.runtime_undiscovered==1);
}
"""
        )


if __name__ == "__main__":
    unittest.main()
