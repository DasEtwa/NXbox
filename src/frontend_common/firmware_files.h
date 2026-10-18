// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace FirmwareManager {
// Decode the fixed-size display version without reading beyond its NUL terminator.
inline std::string DisplayVersion(std::string_view bytes) {
    const auto end = bytes.find('\0');
    if (end == std::string_view::npos) {
        return {};
    }
    const auto text = bytes.substr(0, end);
    unsigned dots = 0;
    bool digit = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c == '.' && digit) {
            ++dots;
            digit = false;
        } else {
            return {};
        }
    }
    return dots == 2 && digit ? std::string(text) : std::string{};
}

inline std::vector<std::filesystem::path> FirmwareFolders(
    const std::filesystem::path& local_state,
    const std::vector<std::filesystem::path>& removable_roots) {
    // Where people put a dump when they skip the instructions: inside the Eden data folder, or
    // straight on the drive root. Only folders named "firmware" are ever archived (renamed).
    std::vector<std::filesystem::path> folders{local_state / "firmware",
                                               local_state / "eden" / "firmware"};
    for (const auto& root : removable_roots) {
        folders.push_back(root / "NXbox" / "firmware");
        folders.push_back(root / "firmware");
    }
    return folders;
}

// A flat TegraExplorer dump. Directories/symlinks cannot masquerade as NCAs.
inline std::vector<std::filesystem::path> FirmwareFiles(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() == ".nca") {
            if (entry.is_symlink() || !entry.is_regular_file()) {
                throw std::filesystem::filesystem_error(
                    "Invalid firmware archive", entry.path(),
                    std::make_error_code(std::errc::invalid_argument));
            }
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Never replace an earlier dump. Keeping the source beside its original folder also works on USB.
template <typename Exists>
inline std::filesystem::path InstalledFolder(const std::filesystem::path& source, Exists exists) {
    auto destination = source.parent_path() / "firmware_installed";
    for (unsigned suffix = 1; exists(destination); ++suffix) {
        destination = source.parent_path() / ("firmware_installed_" + std::to_string(suffix));
    }
    return destination;
}
inline std::filesystem::path InstalledFolder(const std::filesystem::path& source) {
    return InstalledFolder(source, [](const auto& path) { return std::filesystem::exists(path); });
}
inline void ArchiveFirmware(const std::filesystem::path& source) {
    std::filesystem::rename(source, InstalledFolder(source));
}
} // namespace FirmwareManager
