// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>

namespace Service::VI {

// Add a title only after validating game speed, physics and cutscenes on that build.
inline constexpr std::array<std::uint64_t, 0> FrameUnlockTitles{};

inline bool ForceSwapInterval(std::uint64_t program_id, const char* option,
                              std::span<const std::uint64_t> titles = FrameUnlockTitles) {
    // An explicit zero also disables a future per-title default. Reject malformed values.
    if (option) {
        return std::strcmp(option, "1") == 0;
    }
    for (const auto title : titles) {
        if (title == program_id) {
            return true;
        }
    }
    return false;
}

class FrameUnlock {
public:
    struct Observation {
        std::int32_t effective;
        bool changed;
    };

    // Called under the BufferQueue mutex, once for every accepted guest request.
    Observation Observe(std::uint64_t program_id, std::int32_t requested) {
        const bool title_changed = !last_request || title != program_id;
        if (title_changed) {
            title = program_id;
#ifdef NXBOX_UWP
            enabled = ForceSwapInterval(title, std::getenv("NXBOX_FORCE_SWAP_INTERVAL"));
#endif
        }
        const bool changed = title_changed || last_request != requested;
        last_request = requested;
        // Preserve Eden's nonpositive and >=5 speed-control extensions. Only remove
        // ordinary multi-vblank presentation waits, without changing emulated time.
        return {enabled && requested >= 2 && requested <= 4 ? 1 : requested, changed};
    }

private:
    std::uint64_t title{};
    std::optional<std::int32_t> last_request;
    bool enabled{};
};

} // namespace Service::VI
