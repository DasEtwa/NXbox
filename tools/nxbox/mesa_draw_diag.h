/* SPDX-License-Identifier: MIT */
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

inline bool nxbox_draw_diag_enabled() {
   static const bool enabled = [] {
      char value[4] = {};
      return GetEnvironmentVariableA("NXBOX_DRAW_DIAG", value, sizeof(value)) ==
                 1 &&
             value[0] == '1';
   }();
   return enabled;
}

enum NxboxDrawCounter {
   NXBOX_DRAW_CALLS,
   NXBOX_DRAW_ISSUED,
   NXBOX_DRAW_SKIPPED,
   NXBOX_DRAW_FORWARDED,
   NXBOX_DRAW_LINK_FAIL,
   NXBOX_DRAW_DXIL_FAIL,
   NXBOX_DRAW_LOGIC,
   NXBOX_DRAW_DUAL,
   NXBOX_DRAW_CLIP,
   NXBOX_DRAW_POINTS,
   NXBOX_DRAW_POINT_SPRITES,
   NXBOX_DRAW_STENCIL,
   NXBOX_DRAW_RESTART,
   NXBOX_DRAW_INSTANCED,
   NXBOX_DRAW_GS,
   NXBOX_DRAW_SO,
   NXBOX_DRAW_CONSTANT,
   NXBOX_DRAW_INDEPENDENT,
   NXBOX_DRAW_A2C,
   NXBOX_DRAW_SAMPLE_MASK,
   NXBOX_DRAW_DEPTH_BOUNDS,
   NXBOX_DRAW_DEPTH_CLAMP,
   NXBOX_DRAW_VIEWPORT,
   NXBOX_DRAW_SCISSOR,
   NXBOX_DRAW_PREDICATION,
   NXBOX_DRAW_DISCARD,
   NXBOX_DRAW_INSTANCE_ATTR,
   NXBOX_DRAW_DIVISOR_GT1,
   NXBOX_DRAW_BASE_INSTANCE,
   NXBOX_DRAW_INSTANCE_SLOT_CONFLICT,
   NXBOX_DRAW_CLIP_WRITTEN,
   NXBOX_DRAW_CLIP_DISABLED_WRITTEN,
   NXBOX_DRAW_DEPTH_CLIP_ASYMMETRIC,
   NXBOX_DRAW_FRAG_DEPTH,
   NXBOX_DRAW_STENCIL_REF_SPLIT,
   NXBOX_DRAW_STENCIL_REF_UNSUPPORTED,
   NXBOX_DRAW_INDEPENDENT_MRT,
   NXBOX_DRAW_FMT_R10G10B10A2_SNORM,
   NXBOX_DRAW_FMT_R10G10B10A2_SSCALED,
   NXBOX_DRAW_FMT_R10G10B10A2_USCALED,
   NXBOX_DRAW_FMT_B10G10R10A2_UNORM,
   NXBOX_DRAW_FMT_B10G10R10A2_SNORM,
   NXBOX_DRAW_FMT_B10G10R10A2_SSCALED,
   NXBOX_DRAW_FMT_B10G10R10A2_USCALED,
   NXBOX_DRAW_FMT_R8G8B8_SINT,
   NXBOX_DRAW_FMT_R8G8B8_UINT,
   NXBOX_DRAW_FMT_R16G16B16_SINT,
   NXBOX_DRAW_FMT_R16G16B16_UINT,
   NXBOX_DRAW_FMT_R8G8B8A8_SSCALED,
   NXBOX_DRAW_FMT_R8G8B8A8_USCALED,
   NXBOX_DRAW_FMT_R16G16B16A16_SSCALED,
   NXBOX_DRAW_FMT_R16G16B16A16_USCALED,
   NXBOX_DRAW_STENCIL_MASK_SPLIT,
   NXBOX_DRAW_STENCIL_MASK_UNSUPPORTED,
   NXBOX_DRAW_COUNTERS
};

// Inline storage is shared across the driver, linker and WGL present
// translation units.
inline std::atomic<unsigned long long> *nxbox_draw_diag_counts() {
   static std::atomic<unsigned long long> counts[NXBOX_DRAW_COUNTERS]{};
   return counts;
}

inline void nxbox_draw_diag_count(NxboxDrawCounter counter,
                                  bool condition = true) {
   if (condition && nxbox_draw_diag_enabled())
      nxbox_draw_diag_counts()[counter].fetch_add(1, std::memory_order_relaxed);
}

struct NxboxDrawScope {
   const bool enabled = nxbox_draw_diag_enabled();
   bool completed = false;
   explicit NxboxDrawScope() { nxbox_draw_diag_count(NXBOX_DRAW_CALLS); }
   void issued() {
      completed = true;
      nxbox_draw_diag_count(NXBOX_DRAW_ISSUED);
   }
   void forwarded() {
      completed = true;
      nxbox_draw_diag_count(NXBOX_DRAW_FORWARDED);
   }
   void skip(unsigned site) {
      if (enabled && !completed) {
         char text[32];
         std::snprintf(text, sizeof(text), "d3d12_draw_vbo:%u", site);
         SetEnvironmentVariableA("NXBOX_DRAW_SKIP_LAST", text);
      }
   }
   ~NxboxDrawScope() {
      // Every terminal return, including guards injected by earlier patches, is
      // counted.
      nxbox_draw_diag_count(NXBOX_DRAW_SKIPPED, !completed);
   }
};

inline unsigned long long
nxbox_draw_diag_external_delta(const char *name, unsigned long long &previous) {
   char text[64] = {};
   GetEnvironmentVariableA(name, text, sizeof(text));
   const auto current = std::strtoull(text, nullptr, 10);
   const auto delta = current - previous;
   previous = current;
   return delta;
}

inline void nxbox_draw_diag_frame() {
   if (!nxbox_draw_diag_enabled())
      return;
   static unsigned long long frame = 0, gl_previous = 0, swizzle_previous = 0,
                             prim_previous = 0, vbuf_previous = 0;
   unsigned long long values[NXBOX_DRAW_COUNTERS];
   for (unsigned i = 0; i < NXBOX_DRAW_COUNTERS; ++i)
      values[i] =
          nxbox_draw_diag_counts()[i].exchange(0, std::memory_order_relaxed);
   const auto gl_skips =
       nxbox_draw_diag_external_delta("NXBOX_GL_DRAW_SKIP_TOTAL", gl_previous);
   const auto swizzles = nxbox_draw_diag_external_delta(
       "NXBOX_GL_SWIZZLE_TOTAL", swizzle_previous);
   const auto aux_skips =
       nxbox_draw_diag_external_delta("NXBOX_PRIMCONVERT_DRAW_SKIP_TOTAL",
                                      prim_previous) +
       nxbox_draw_diag_external_delta("NXBOX_VBUF_DRAW_SKIP_TOTAL",
                                      vbuf_previous);
   char text[4096];
   size_t used = std::snprintf(
       text, sizeof(text),
       "frame=%llu gl_skipped=%llu aux_skipped=%llu viewport_swizzle=%llu",
       ++frame, gl_skips, aux_skips, swizzles);
   const char *names[]{"calls",
                       "issued",
                       "skipped",
                       "forwarded",
                       "link_fail",
                       "dxil_fail",
                       "logic",
                       "dual",
                       "clip",
                       "points",
                       "point_sprites",
                       "stencil",
                       "restart",
                       "instanced",
                       "gs",
                       "so",
                       "constant",
                       "independent",
                       "a2c",
                       "sample_mask",
                       "depth_bounds",
                       "depth_clamp",
                       "viewport",
                       "scissor",
                       "predication",
                       "discard",
                       "instance_attr",
                       "divisor_gt1",
                       "base_instance",
                       "instance_slot_conflict",
                       "clip_written",
                       "clip_disabled_written",
                       "depth_clip_asymmetric",
                       "frag_depth",
                       "stencil_ref_split",
                       "stencil_ref_unsupported",
                       "independent_mrt",
                       "fmt_r10g10b10a2_snorm",
                       "fmt_r10g10b10a2_sscaled",
                       "fmt_r10g10b10a2_uscaled",
                       "fmt_b10g10r10a2_unorm",
                       "fmt_b10g10r10a2_snorm",
                       "fmt_b10g10r10a2_sscaled",
                       "fmt_b10g10r10a2_uscaled",
                       "fmt_r8g8b8_sint",
                       "fmt_r8g8b8_uint",
                       "fmt_r16g16b16_sint",
                       "fmt_r16g16b16_uint",
                       "fmt_r8g8b8a8_sscaled",
                       "fmt_r8g8b8a8_uscaled",
                       "fmt_r16g16b16a16_sscaled",
                       "fmt_r16g16b16a16_uscaled",
                       "stencil_mask_split",
                       "stencil_mask_unsupported"};
   static_assert(sizeof(names) / sizeof(names[0]) == NXBOX_DRAW_COUNTERS,
                 "counter names");
   for (unsigned i = 0; i < NXBOX_DRAW_COUNTERS; ++i)
      used += std::snprintf(text + used, sizeof(text) - used, " %s=%llu",
                            names[i], values[i]);
   SetEnvironmentVariableA("NXBOX_D3D12_DRAW_DIAG", text);
}
