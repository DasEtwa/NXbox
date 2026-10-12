// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/nxbox_shader_probe.h"

namespace NxboxShaderProbe {
inline constexpr std::array<std::string_view, 6> GuestStages{"va", "vb", "tcs", "tes", "gs", "fs"};
inline constexpr std::array<std::string_view, 5> HostStages{"vertex", "tcs", "tes", "gs", "fs"};

inline std::filesystem::path PipelineFolder(const std::filesystem::path& root, Hash hash) {
    return root / Hex(hash);
}

inline std::filesystem::path StageFile(const std::filesystem::path& root, Hash hash,
                                       std::size_t stage, std::string_view extension) {
    return PipelineFolder(root, hash) /
           (std::string(HostStages.at(stage)) + std::string(extension));
}

inline std::optional<std::string> ReadReplacement(const std::filesystem::path& root, Hash hash,
                                                  std::size_t stage) {
    if (root.empty()) {
        return std::nullopt;
    }
    const auto path = StageFile(root, hash, stage, ".glsl");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (std::filesystem::exists(path)) {
            throw std::runtime_error("Unreadable shader replacement");
        }
        return std::nullopt;
    }
    std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad() || text.empty()) {
        throw std::runtime_error("Unreadable or empty shader replacement");
    }
    return text;
}

// Immutable driver inputs owned by each UWP cache entry, independent of worker completion.
struct DumpArtifacts {
    struct File {
        std::string name;
        std::string bytes;
    };
    std::vector<File> files;
    std::size_t stages{};
    std::mutex mutex;
    bool written{};

    void Add(std::string name, const void* data, std::size_t size) {
        files.push_back({std::move(name),
                         size ? std::string(static_cast<const char*>(data), size) : std::string{}});
    }

    // Success is published only after every file closes successfully; failures remain retryable.
    std::size_t Write(const std::filesystem::path& root, Hash hash) const {
        const auto folder = PipelineFolder(root, hash);
        std::filesystem::create_directories(folder);
        std::size_t bytes{};
        for (const auto& entry : files) {
            std::ofstream file(folder / entry.name, std::ios::binary | std::ios::trunc);
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.write(entry.bytes.data(), static_cast<std::streamsize>(entry.bytes.size()));
            file.close();
            bytes += entry.bytes.size();
        }
        // A replacement can change the backend for one stage on a later run.
        // Remove obsolete host formats so every remaining source describes this capture.
        for (const auto stage : HostStages) {
            for (const auto extension : {".glsl", ".spv", ".glasm"}) {
                const auto name = std::string(stage) + extension;
                if (std::none_of(files.begin(), files.end(),
                                 [&](const File& entry) { return entry.name == name; })) {
                    std::filesystem::remove(folder / name);
                }
            }
        }
        return bytes;
    }
};
} // namespace NxboxShaderProbe
