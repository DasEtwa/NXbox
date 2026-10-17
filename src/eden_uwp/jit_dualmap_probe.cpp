// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_uwp/jit_dualmap_probe.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <windows.h>

#include <memoryapi.h>

#include "eden_uwp/diagnostic.h"

namespace {

constexpr SIZE_T kSectionSize = 64 * 1024;
constexpr int kIterations = 10000;

using MapViewOfFile3FromAppFn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG,
                                               ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

std::string Err(const char* what = nullptr) {
    const auto code = GetLastError();
    return std::string{what ? what : "err"} + ":" + std::to_string(code);
}

// mov eax, imm32 ; ret
void WriteStub(void* dst, std::uint32_t value) {
    std::uint8_t code[16] = {0xB8, 0, 0, 0, 0, 0xC3};
    std::memcpy(code + 1, &value, sizeof(value));
    std::memset(code + 6, 0xCC, sizeof(code) - 6);
    std::memcpy(dst, code, sizeof(code));
}

// Plain-old-data only: __try cannot share a function with objects that need unwinding.
int CallProtected(void* entry, bool* faulted) {
    *faulted = false;
    __try {
        return reinterpret_cast<int (*)()>(entry)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return -1;
    }
}

const char* Exec(void* entry, int expected) {
    bool faulted = false;
    const int got = CallProtected(entry, &faulted);
    return faulted ? "fault" : (got == expected ? "ok" : "wrong");
}

double MicrosSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace

void NxboxJitDualMapProbe() noexcept {
    try {
        using EdenXbox::Diagnostic;
        std::string create = "none", rw = "skipped", rx = "skipped", exec1 = "skipped",
                    exec2 = "skipped";
        long long alias_us = -1, protect_us = -1;
        HANDLE section = nullptr;
        void* rw_view = nullptr;
        void* rx_view = nullptr;
        const HANDLE process = GetCurrentProcess();

        struct Attempt {
            const char* name;
            DWORD protect;
        };
        for (const Attempt attempt : {Attempt{"PAGE_EXECUTE_READWRITE", PAGE_EXECUTE_READWRITE},
                                      Attempt{"PAGE_READWRITE|SEC_COMMIT", PAGE_READWRITE | SEC_COMMIT},
                                      Attempt{"PAGE_EXECUTE_READWRITE|SEC_COMMIT",
                                              PAGE_EXECUTE_READWRITE | SEC_COMMIT}}) {
            section = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, attempt.protect,
                                               kSectionSize, nullptr);
            create = section ? "ok" : Err();
            Diagnostic(std::string{"JIT_DUALMAP_PROBE attempt create "} + attempt.name + " -> " +
                       create);
            if (section) {
                break;
            }
        }

        if (section) {
            rw_view = MapViewOfFileFromApp(section, FILE_MAP_WRITE, 0, kSectionSize);
            rw = rw_view ? "ok" : Err();
            Diagnostic("JIT_DUALMAP_PROBE attempt map rw (FILE_MAP_WRITE) -> " + rw);

            if (rw_view) {
                const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
                const auto map3 = kernelbase ? reinterpret_cast<MapViewOfFile3FromAppFn>(
                                                   GetProcAddress(kernelbase, "MapViewOfFile3FromApp"))
                                             : nullptr;
                if (map3) {
                    rx_view = map3(section, process, nullptr, 0, 0, 0, PAGE_EXECUTE_READ, nullptr, 0);
                    rx = rx_view ? "ok:mapviewoffile3" : Err();
                } else {
                    rx = "err:no_MapViewOfFile3FromApp";
                }
                Diagnostic("JIT_DUALMAP_PROBE attempt map rx (MapViewOfFile3FromApp) -> " + rx);
                if (!rx_view) {
                    rx_view = MapViewOfFileFromApp(section, FILE_MAP_EXECUTE | FILE_MAP_READ, 0,
                                                   kSectionSize);
                    rx = rx_view ? "ok:mapviewoffile" : Err();
                    Diagnostic("JIT_DUALMAP_PROBE attempt map rx (FILE_MAP_EXECUTE|FILE_MAP_READ) -> " +
                               rx);
                }
            }
        }

        if (rw_view && rx_view) {
            WriteStub(rw_view, 0x2A);
            FlushInstructionCache(process, rx_view, 16);
            exec1 = Exec(rx_view, 0x2A);

            WriteStub(rw_view, 0x2B);
            FlushInstructionCache(process, rx_view, 16);
            exec2 = Exec(rx_view, 0x2B);

            if (exec1 == "ok") {
                const auto alias_start = std::chrono::steady_clock::now();
                for (int i = 0; i < kIterations; ++i) {
                    WriteStub(rw_view, 0x2A + (i & 1));
                    FlushInstructionCache(process, rx_view, 16);
                }
                alias_us = static_cast<long long>(MicrosSince(alias_start));
            }
        }

        // The current scheme: one page flipped RW -> RX -> RW around every write.
        if (void* page = VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE)) {
            DWORD old = 0;
            bool ok = true;
            const auto protect_start = std::chrono::steady_clock::now();
            for (int i = 0; i < kIterations && ok; ++i) {
                WriteStub(page, 0x2A);
                ok = VirtualProtectFromApp(page, 4096, PAGE_EXECUTE_READ, &old) != FALSE;
                if (ok) {
                    FlushInstructionCache(process, page, 16);
                    ok = VirtualProtectFromApp(page, 4096, PAGE_READWRITE, &old) != FALSE;
                }
            }
            if (ok) {
                protect_us = static_cast<long long>(MicrosSince(protect_start));
            }
            VirtualFree(page, 0, MEM_RELEASE);
        }

        if (rx_view) {
            UnmapViewOfFile(rx_view);
        }
        if (rw_view) {
            UnmapViewOfFile(rw_view);
        }
        if (section) {
            CloseHandle(section);
        }

        Diagnostic("JIT_DUALMAP_PROBE create=" + create + " rw=" + rw + " rx=" + rx +
                   " exec1=" + exec1 + " exec2=" + exec2 + " alias_us=" + std::to_string(alias_us) +
                   " protect_us=" + std::to_string(protect_us));
    } catch (...) {
        // A failed probe must never stop the game from booting.
    }
}
