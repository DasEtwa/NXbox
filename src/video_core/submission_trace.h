// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <fmt/format.h>
#include "common/logging.h"

namespace VideoCommon::SubmissionTrace {
inline std::atomic<std::uint64_t> frame{1};
inline std::atomic<std::uint64_t> events{0};
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_SUBMISSION_TRACE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}
inline void Reset() {
    if (!Enabled())
        return;
    events.store(0, std::memory_order_relaxed);
    frame.store(1, std::memory_order_relaxed);
}
inline bool Active() {
    if (!Enabled())
        return false;
    static const std::uint64_t selected = [] {
        const char* value = std::getenv("NXBOX_D3D12_DRAW_TRACE_FRAME");
        std::uint64_t selected_frame = 0;
        if (value) {
            const std::string_view text{value};
            const auto parsed =
                std::from_chars(text.data(), text.data() + text.size(), selected_frame);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return std::uint64_t{0};
        }
        return selected_frame;
    }();
    return selected && frame.load(std::memory_order_relaxed) == selected;
}
template <class... Args>
void Log(fmt::format_string<Args...> format, Args&&... args) {
    if (!Active())
        return;
    const auto event = events.fetch_add(1, std::memory_order_relaxed);
    if (event < 20000)
        LOG_INFO(Render, "NXBOX_SUBMISSION frame={} event={} {}", frame.load(), event,
                 fmt::format(format, std::forward<Args>(args)...));
    else if (event == 20000)
        LOG_WARNING(Render, "NXBOX_SUBMISSION frame={} TRUNCATED limit=20000", frame.load());
}
inline void EndFrame(std::uint64_t next_frame) {
    if (!Enabled())
        return;
    if (Active())
        LOG_INFO(Render, "NXBOX_SUBMISSION frame={} frame_end events={} truncated={}", frame.load(),
                 events.load(), events.load() > 20000);
    frame.store(next_frame, std::memory_order_relaxed);
}
} // namespace VideoCommon::SubmissionTrace
