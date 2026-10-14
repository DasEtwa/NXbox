// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef NXBOX_UWP
// Avoid windows.h in OpenGL translation units.
extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char*, const char*);
#endif

namespace NxboxUpload {
// Set by the buffer-cache page watch: trace exactly the next backend copy.
inline std::atomic<bool> trace_next_copy{false};
enum class Event {
    AstcQueued,
    AstcUploaded,
    AstcPendingBinding,
    PipelinePending,
    PipelineSkipped,
    StagingBusyAvoided,
    StagingBusyMap,
    BufferInvalidation,
    BufferUpload,
    ExpandedFlush,
    Count,
};

inline bool Enabled() {
#if defined(NXBOX_UWP) || defined(NXBOX_UPLOAD_DIAG_MESA)
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_UPLOAD_DIAG");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
#else
    return false;
#endif
}

inline auto& Counts() {
    static std::array<std::atomic<uint64_t>, static_cast<size_t>(Event::Count)> counts{};
    return counts;
}

inline void Publish() {
#if defined(NXBOX_UWP) || defined(NXBOX_UPLOAD_DIAG_MESA)
    if (!Enabled())
        return;
    static std::atomic<uint64_t> samples{0};
    const auto sample = samples.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sample > 16 && sample % 4096)
        return;
    char text[768];
    constexpr const char* names[]{"astc_queued",      "astc_uploaded",       "astc_pending_binding",
                                  "pipeline_pending", "pipeline_skipped",    "staging_busy_avoided",
                                  "staging_busy_map", "buffer_invalidation", "buffer_upload",
                                  "expanded_flush"};
    size_t offset = 0;
    for (size_t i = 0; i < Counts().size(); ++i) {
        offset += std::snprintf(
            text + offset, sizeof(text) - offset, "%s%s=%llu", i ? " " : "", names[i],
            static_cast<unsigned long long>(Counts()[i].load(std::memory_order_relaxed)));
    }
#ifdef NXBOX_UPLOAD_DIAG_MESA
    SetEnvironmentVariableA("NXBOX_MESA_UPLOAD_READY", text);
#else
    SetEnvironmentVariableA("NXBOX_GL_UPLOAD_READY", text);
#endif
#endif
}

inline void Record(Event event) {
    if (!Enabled())
        return;
    Counts()[static_cast<size_t>(event)].fetch_add(1, std::memory_order_relaxed);
    Publish();
}

inline bool ForceAstcSync() {
    const char* value = std::getenv("NXBOX_ASTC_SYNC");
    return value && value[0] == '1' && value[1] == '\0';
}
} // namespace NxboxUpload
