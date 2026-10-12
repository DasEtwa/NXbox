// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef NXBOX_UWP
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <winrt/Windows.Storage.h>
#include "common/nxbox_shader_dump.h"
#include "eden_uwp/diagnostic.h"

namespace NxboxShaderProbe {
inline const Config& Settings() {
    static const Config config = [] {
        Config result;
        const auto env = [](const char* name) -> std::string_view {
            const char* value = std::getenv(name);
            return value ? value : "";
        };
        result.ids = env("NXBOX_SHADER_IDS") == "1";
        const auto invalid = [](const char* name) {
            EdenXbox::Diagnostic(std::string("SHADER_PROBE invalid ") + name + "; ignored");
        };
        if (!env("NXBOX_SHADER_IDS_EVERY").empty() &&
            (!Number(env("NXBOX_SHADER_IDS_EVERY"), result.every, 10) || result.every == 0)) {
            result.every = 60;
            invalid("NXBOX_SHADER_IDS_EVERY");
        }
        if (!result.skip.Parse(env("NXBOX_SKIP_PIPELINES"))) {
            invalid("NXBOX_SKIP_PIPELINES");
        }
        if (!env("NXBOX_SKIP_PIPELINE_HALF").empty() &&
            !result.bucket.Parse(env("NXBOX_SKIP_PIPELINE_HALF"))) {
            invalid("NXBOX_SKIP_PIPELINE_HALF");
        }
        if (!env("NXBOX_SKIP_PIPELINE_CYCLE").empty() &&
            !result.cycle.Parse(env("NXBOX_SKIP_PIPELINE_CYCLE"))) {
            invalid("NXBOX_SKIP_PIPELINE_CYCLE");
        }
        if (!env("NXBOX_SKIP_PIPELINE_CYCLE_WITHIN").empty() &&
            !result.cycle.ParseWithin(env("NXBOX_SKIP_PIPELINE_CYCLE_WITHIN"))) {
            invalid("NXBOX_SKIP_PIPELINE_CYCLE_WITHIN");
        }
        if (!result.dump.Parse(env("NXBOX_DUMP_SHADERS"), false)) {
            invalid("NXBOX_DUMP_SHADERS");
        }
        return result;
    }();
    return config;
}

inline Frames& FrameState() {
    static Frames frames;
    return frames;
}

inline CycleState& CycleFrameState() {
    static CycleState state;
    return state;
}

inline Hash CycleTick() {
    return static_cast<Hash>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}

inline void BeginCycleFrame(const Config& config) {
    if (config.cycle.count == 0) {
        return;
    }
    auto& state = CycleFrameState();
    if (state.frame_begun) {
        return;
    }
    state.frame_begun = true;
    if (state.Advance(config.cycle, CycleTick())) {
        EdenXbox::Diagnostic("SKIP_CYCLE phase=" + std::to_string(state.phase) +
                             " start_tick=" + std::to_string(state.start_tick) +
                             " buckets=" + std::to_string(config.cycle.count));
    }
}

inline std::filesystem::path DumpFolder() {
    return std::filesystem::path{
               winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()} /
           "shader_dump";
}

// Dump selection is deliberately independent of the lifetime-cached draw controls.
// A cache hit can service a selection installed after pipeline creation.
inline bool DumpSelected(Hash hash, const std::array<Hash, 6>& stages) {
    thread_local std::string previous;
    thread_local Config config;
    const char* value = std::getenv("NXBOX_DUMP_SHADERS");
    const std::string_view text = value ? value : "";
    if (text != previous) {
        previous = text;
        config.dump.ranges.clear();
        if (!config.dump.Parse(text, false)) {
            EdenXbox::Diagnostic("SHADER_PROBE invalid NXBOX_DUMP_SHADERS; ignored");
        }
    }
    return config.Dump(hash, stages);
}

inline std::filesystem::path ReplacementFolder() {
    const char* value = std::getenv("NXBOX_SHADER_REPLACE_DIR");
    if (!value || !*value || std::string_view(value) == "0") {
        return {};
    }
    const auto local = DumpFolder().parent_path();
    if (std::string_view(value) == "1") {
        return local / "shader_replace";
    }
    const std::filesystem::path path{value};
    return path.is_absolute() ? path : local / path;
}

inline void DumpPipeline(DumpArtifacts& artifacts, Hash hash,
                         const std::array<Hash, 6>& stages) noexcept {
    try {
        if (!DumpSelected(hash, stages)) {
            return;
        }
        const std::lock_guard lock(artifacts.mutex);
        if (artifacts.written) {
            return;
        }
        const auto bytes = artifacts.Write(DumpFolder(), hash);
        artifacts.written = true;
        EdenXbox::Diagnostic("SHADER_DUMP hash=" + Hex(hash) + " stages=" +
                             std::to_string(artifacts.stages) + " bytes=" + std::to_string(bytes));
    } catch (...) {
        EdenXbox::Diagnostic("SHADER_DUMP hash=" + Hex(hash) + " stages=0 bytes=0 failed");
    }
}
} // namespace NxboxShaderProbe
#endif
