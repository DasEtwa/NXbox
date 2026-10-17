// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs Xbyak-generated x86-64 code through an RX alias while every store goes to the RW alias of the
// same shared-memory object. This is the host-side model of the UWP dual-mapped JIT cache
// (NXBOX_JIT_DUALMAP=1). It needs the patched Xbyak (setExecDelta). Xbyak emits x86-64 regardless of
// the host, so on non-x86 hosts build with -DXBYAK64_GCC.
//
// Execution is only attempted on x86-64 Linux. Everywhere else the alias is mapped read-only and the
// test checks the emitted bytes, displacements and addresses instead.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <xbyak/xbyak.h>

#ifndef XBYAK_NXBOX_EXEC_DELTA
#error "Xbyak is not patched with 0001-exec-delta-dualmap.patch"
#endif

namespace {

constexpr size_t kSize = 1 << 20;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

int CreateSharedFile() {
#ifdef __linux__
    return static_cast<int>(syscall(SYS_memfd_create, "nxbox-dualmap", 0));
#else
    char name[64];
    std::snprintf(name, sizeof(name), "/nxbox-dm-%d", static_cast<int>(getpid()));
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        shm_unlink(name);
    }
    return fd;
#endif
}

using U8 = std::uint8_t;

#if defined(__linux__) && defined(__x86_64__)
constexpr bool kExecute = true;
#else
constexpr bool kExecute = false;
#endif

struct Gen : Xbyak::CodeGenerator {
    Gen(size_t size, void* rw_buffer) : Xbyak::CodeGenerator(size, rw_buffer) {}
};

template <class R>
R Call(const void* exec_address) {
    return reinterpret_cast<R (*)()>(const_cast<void*>(exec_address))();
}

} // namespace

int main() {
    const int fd = CreateSharedFile();
    if (fd < 0 || ftruncate(fd, kSize) != 0) {
        std::puts("SKIP: cannot create a shared memory object");
        return 0;
    }
    U8* const rw = static_cast<U8*>(mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    U8* const rx = static_cast<U8*>(mmap(nullptr, kSize, PROT_READ | (kExecute ? PROT_EXEC : 0), MAP_SHARED, fd, 0));
    if (rw == MAP_FAILED || rx == MAP_FAILED) {
        std::puts("SKIP: cannot map the same object RW and RX on this host");
        return 0;
    }
    CHECK(rw != rx);

    Gen g(kSize, rw);
    g.setExecDelta(rx - rw);
    CHECK(g.getCode() == rx);
    CHECK(g.getCurr() == rx);
    using namespace Xbyak::util;

    // A: returns 41.
    const U8* const a = g.getCurr();
    g.mov(eax, 41);
    g.ret();

    // B: absolute backward CALL to A (rel32 against the executable cursor), returns 42.
    g.align(16);
    const U8* const b = g.getCurr();
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 16 == 0);
    g.sub(rsp, 8);
    g.call(a);
    g.add(rsp, 8);
    g.add(eax, 1);
    g.ret();

    // C: forward label jump skips an instruction, returns 7.
    g.align(16);
    const U8* const c = g.getCurr();
    {
        Xbyak::Label skip;
        g.xor_(eax, eax);
        g.jmp(skip, Xbyak::CodeGenerator::T_NEAR);
        g.mov(eax, 99);
        g.L(skip);
        const U8* const at_label = g.getCurr();
        g.add(eax, 7);
        g.ret();
        CHECK(skip.getAddress() == at_label);
    }

    // D: absolute jmp(const void*) tail-jumps to A, returns 41.
    g.align(16);
    const U8* const d = g.getCurr();
    g.jmp(a);

    // E: rip-relative load of a constant addressed by its executable pointer, the way
    // ConstantPool::GetConstant does it (qword[rip + pointer]).
    g.align(16);
    const U8* const constant = g.getCurr();
    g.dq(0x1122334455667788ULL);
    g.align(16);
    const U8* const e = g.getCurr();
    g.mov(rax, g.qword[g.rip + constant]);
    g.ret();

    // F: rip-relative load through a Label, then an absolute label address stored by putL and used
    // for an indirect jump; the value read must be an executable address.
    g.align(16);
    const U8* const f = g.getCurr();
    {
        Xbyak::Label table, target;
        g.mov(rax, g.ptr[g.rip + table]);
        g.jmp(rax);
        g.L(target);
        g.mov(eax, 1234);
        g.ret();
        g.align(8);
        g.L(table);
        g.putL(target);
        CHECK(target.getAddress() != nullptr);
        std::uint64_t stored = 0;
        std::memcpy(&stored, rw + (table.getAddress() - rx), sizeof(stored));
        CHECK(stored == reinterpret_cast<std::uintptr_t>(target.getAddress()));
        CHECK(stored >= reinterpret_cast<std::uintptr_t>(rx));
        CHECK(stored < reinterpret_cast<std::uintptr_t>(rx) + kSize);
    }

    // G: a patch site. Starts as `jmp A`, is later rewritten in place (SetCodePtr style) to `jmp B`.
    g.align(16);
    const U8* const site = g.getCurr();
    g.jmp(a);
    g.nop(8);
    const size_t after_site = g.getSize();

    // The relative displacement must have been computed from executable addresses.
    {
        std::int32_t rel = 0;
        std::memcpy(&rel, rw + (site - rx) + 1, sizeof(rel));
        CHECK(static_cast<std::intptr_t>(rel) == a - (site + 5));
    }

    // Everything the generator handed out lies in the RX view; the bytes seen through both views agree.
    for (const U8* p : {a, b, c, d, constant, e, f, site}) {
        CHECK(p >= rx && p < rx + kSize);
    }
    CHECK(std::memcmp(rw, rx, g.getSize()) == 0);

    // Execute through the RX alias.
    if (kExecute) {
        CHECK(Call<int>(a) == 41);
        CHECK(Call<int>(b) == 42);
        CHECK(Call<int>(c) == 7);
        CHECK(Call<int>(d) == 41);
        CHECK(Call<std::uint64_t>(e) == 0x1122334455667788ULL);
        CHECK(Call<int>(f) == 1234);
        CHECK(Call<int>(site) == 41);
    }

    // Rewrite already-emitted code through the RW view only, then run the new bytes through RX.
    auto site_target = [&] {
        std::int32_t rel = 0;
        std::memcpy(&rel, rx + (site - rx) + 1, sizeof(rel));  // read back through the RX view
        return site + 5 + rel;
    };
    g.setSize(site - rx);
    g.jmp(b);
    g.setSize(after_site);
    CHECK(site_target() == b);
    if (kExecute) {
        CHECK(Call<int>(site) == 42);
    }

    // Restore the patch back (the Unpatch direction) the same way.
    g.setSize(site - rx);
    g.jmp(a);
    g.setSize(after_site);
    CHECK(site_target() == a);
    if (kExecute) {
        CHECK(Call<int>(site) == 41);
    }

    // Stock behavior is untouched: delta 0 means addresses are the write addresses.
    {
        static U8 plain[256];
        Gen s(sizeof(plain), plain);
        CHECK(s.getExecDelta() == 0);
        CHECK(s.getCode() == plain);
        s.nop(3);
        CHECK(s.getCurr() == plain + 3);
    }

    munmap(rw, kSize);
    munmap(rx, kSize);
    close(fd);
    if (g_failures == 0) {
        std::puts("xbyak dualmap: ok");
    }
    return g_failures == 0 ? 0 : 1;
}
