// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdio>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char*, char*,
                                                                                 unsigned long);
extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char*, const char*);

inline bool nxbox_guest_draw_diag_enabled() {
    static const bool enabled = [] {
        char value[4]{};
        return GetEnvironmentVariableA("NXBOX_DRAW_DIAG", value, sizeof(value)) == 1 &&
               value[0] == '1';
    }();
    return enabled;
}

inline void nxbox_record_guest_viewport_swizzle(bool non_identity) {
    if (!nxbox_guest_draw_diag_enabled() || !non_identity) {
        return;
    }
    static std::atomic<unsigned long long> total{0};
    char text[64];
    std::snprintf(text, sizeof(text), "%llu", total.fetch_add(1, std::memory_order_relaxed) + 1);
    SetEnvironmentVariableA("NXBOX_GL_SWIZZLE_TOTAL", text);
}
#endif
