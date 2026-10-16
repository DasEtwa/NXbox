// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eden_uwp/title_settings.h"

namespace EdenXbox::RendererSettings {
// Values of Eden's RendererBackend: OpenGL_GLSL = 0, D3D12 = 5.
inline constexpr int kOpenGL = 0;
inline constexpr int kDirectX12 = 5;
inline int Read(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    int backend = kDirectX12;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && (entry->label == "renderer_backend" || entry->label == "backend")) {
            // Mesa also supports the legacy GLASM and SPIR-V settings.
            backend = entry->value == "0" || entry->value == "3" || entry->value == "4"
                          ? kOpenGL
                          : kDirectX12;
        }
    }
    return backend;
}
inline bool Save(const std::filesystem::path& local, int backend) {
    if (backend != kDirectX12 && backend != kOpenGL)
        return false;
    const auto file = local / "eden_settings.txt";
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return false;
    std::ifstream in(file);
    if (exists && !in)
        return false;
    std::string content;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (!entry || (entry->label != "renderer_backend" && entry->label != "backend"))
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "renderer_backend=" << backend << '\n';
    out.close();
    return !out.fail();
}
} // namespace EdenXbox::RendererSettings
