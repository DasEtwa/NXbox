// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef NXBOX_UWP
#include <filesystem>
#include <fstream>
#include <mutex>

#include "common/nxbox_shader_probe.h"
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

inline std::filesystem::path DumpFolder() {
    return std::filesystem::path{
               winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()} /
           "shader_dump";
}

inline void WriteDump(const std::string& name, const void* data, std::size_t size) noexcept {
    // Disk-cache reconstruction may generate different pipelines concurrently.
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    try {
        const auto folder = DumpFolder();
        std::filesystem::create_directories(folder);
        std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        file.close();
        EdenXbox::Diagnostic("SHADER_DUMP " + name + (file ? " ok" : " failed"));
    } catch (...) {
        EdenXbox::Diagnostic("SHADER_DUMP " + name + " failed");
    }
}

inline void WriteText(const std::string& name, const std::string& text) {
    WriteDump(name, text.data(), text.size());
}
} // namespace NxboxShaderProbe
#endif
