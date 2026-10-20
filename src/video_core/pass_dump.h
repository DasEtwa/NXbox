// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>

namespace VideoCommon::PassDump {
// Probe the actual destination before waiting for the selected frame. The marker also
// lets the collector distinguish a writable destination from an uncollected directory.
inline void PrepareDirectory(const std::filesystem::path& directory, std::string_view frame) {
    std::filesystem::create_directories(directory);
    std::ofstream marker;
    marker.exceptions(std::ios::failbit | std::ios::badbit);
    marker.open(directory / "capture.txt", std::ios::trunc);
    marker << "NXBOX_PASS_DUMP_FRAME=" << frame << '\n';
    marker.close();
}
} // namespace VideoCommon::PassDump
