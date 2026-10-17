// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: 0BSD
#include <cstdio>

#if defined(_WIN32) && defined(_M_X64)
#    include <windows.h>
#endif

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#if defined(_WIN32) && defined(_M_X64)
namespace {
bool trace_native_faults = false;
LONG CALLBACK ReportNativeFault(EXCEPTION_POINTERS* fault) {
    if (!trace_native_faults || fault->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto& context = *fault->ContextRecord;
    HMODULE module = nullptr;
    char module_name[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(context.Rip), &module);
    if (module) {
        GetModuleFileNameA(module, module_name, MAX_PATH);
    }
    std::fprintf(stderr, "Native access violation: rip=%llx rsp=%llx address=%llx module=%s offset=%llx\n",
                 context.Rip, context.Rsp, static_cast<unsigned long long>(fault->ExceptionRecord->ExceptionInformation[1]),
                 module ? module_name : "generated code", context.Rip - reinterpret_cast<unsigned long long>(module));
    std::fprintf(stderr, "rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx rsi=%llx rdi=%llx\n",
                 context.Rax, context.Rcx, context.Rdx, context.R8, context.R9, context.Rsi, context.Rdi);
    unsigned char bytes[16]{};
    SIZE_T bytes_read = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Rip), bytes, sizeof(bytes), &bytes_read)) {
        std::fprintf(stderr, "Host instruction bytes:");
        for (SIZE_T i = 0; i < bytes_read; ++i) {
            std::fprintf(stderr, " %02x", static_cast<unsigned>(bytes[i]));
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // Anonymous namespace
#endif

class TestTraceListener final : public Catch::EventListenerBase {
public:
    using EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override {
#if defined(_WIN32) && defined(_M_X64)
        static const auto observer = AddVectoredExceptionHandler(0, ReportNativeFault);
        (void)observer;
        trace_native_faults = info.name == "A32: Large random arm block";
#endif
        std::printf("Running test: %s\n", info.name.c_str());
        std::fflush(stdout);
    }
};

CATCH_REGISTER_LISTENER(TestTraceListener)
