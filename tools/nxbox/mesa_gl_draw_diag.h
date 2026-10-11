/* SPDX-License-Identifier: MIT */
#pragma once

#ifdef _WIN32
#include <stdint.h>
#include <stdio.h>
#include "util/u_atomic.h"
/* src/mesa/main must not include <windows.h> (dd.h #errors), so declare the two kernel32
 * entry points this header needs. */
#ifndef _WINDOWS_
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char *, char *,
                                                                     unsigned long);
__declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char *, const char *);
#ifdef __cplusplus
}
#endif
#endif
#ifndef NXBOX_DRAW_SKIP_KEY
#define NXBOX_DRAW_SKIP_KEY "NXBOX_GL_DRAW_SKIP_TOTAL"
#define NXBOX_DRAW_SKIP_LAST_KEY "NXBOX_GL_DRAW_SKIP_LAST"
#endif

static inline void nxbox_gl_draw_skip(const char *function, unsigned line) {
   char value[4] = {0};
   static int64_t total = 0;
   if (GetEnvironmentVariableA("NXBOX_DRAW_DIAG", value, sizeof(value)) != 1 ||
       value[0] != '1')
      return;
   char text[160];
   snprintf(text, sizeof(text), "%llu",
            (unsigned long long)p_atomic_inc_return(&total));
   SetEnvironmentVariableA(NXBOX_DRAW_SKIP_KEY, text);
   snprintf(text, sizeof(text), "%s:%u", function, line);
   SetEnvironmentVariableA(NXBOX_DRAW_SKIP_LAST_KEY, text);
}
#else
static inline void nxbox_gl_draw_skip(const char *function, unsigned line) {
   (void)function;
   (void)line;
}
#endif
