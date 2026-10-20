// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <filesystem>
#include <vector>

namespace EdenXbox::Ui {

// The legacy LocalState/eden/load root remains owned by Eden's filesystem controller.
inline std::vector<std::filesystem::path> ModLoadPaths(
    const std::filesystem::path& local_state,
    const std::vector<std::filesystem::path>& game_folders) {
    std::vector<std::filesystem::path> paths{local_state / "load"};
    for (const auto& folder : game_folders) {
        const auto path = (folder / "load").lexically_normal();
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }
    return paths;
}

} // namespace EdenXbox::Ui
