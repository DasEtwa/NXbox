// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable cache I/O so migration and bitmap bytes can be exercised on a host.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace EdenXbox::Ui {
namespace fs = std::filesystem;
namespace Art {
inline bool WriteBytes(const fs::path& file, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    auto partial = file;
    partial += ".partial";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        fs::remove(partial, ec);
        return false;
    }
    fs::remove(file, ec); // Cache replacement; only complete bytes are ever renamed into place.
    fs::rename(partial, file, ec);
    if (!ec)
        return true;
    fs::remove(partial, ec);
    return false;
}

// Keep the previous validated index through replacement errors and interrupted refreshes.
inline bool ReplaceKeepingOld(const fs::path& file, const std::vector<std::uint8_t>& bytes) {
    static std::mutex mutex;
    const std::lock_guard<std::mutex> lock(mutex);
    auto next = file;
    next += ".next";
    auto previous = file;
    previous += ".previous";
    if (!WriteBytes(next, bytes))
        return false;
    std::error_code ec;
    const bool had_old = fs::exists(file, ec);
    if (ec)
        return false;
    if (had_old) {
        fs::remove(previous, ec);
        if (ec)
            return false;
        fs::rename(file, previous, ec);
        if (ec)
            return false;
    }
    fs::rename(next, file, ec);
    if (ec) {
        if (had_old)
            fs::rename(previous, file, ec);
        return false;
    }
    fs::remove(previous, ec);
    return true;
}

inline std::vector<std::uint8_t> EncodeBitmap(const std::vector<std::uint8_t>& bgra,
                                              std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > 4096 || height > 4096 ||
        bgra.size() != std::size_t(width) * height * 4 || bgra.size() > (16u << 20))
        return {};
    // BITMAPINFOHEADER, top-down 32-bit BGRA; generated art and control JPEGs are opaque.
    std::vector<std::uint8_t> bytes(54 + bgra.size());
    const auto word = [&](std::size_t at, std::uint32_t value, unsigned count = 4) {
        for (unsigned i = 0; i < count; ++i)
            bytes[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    bytes[0] = 'B';
    bytes[1] = 'M';
    word(2, static_cast<std::uint32_t>(bytes.size()));
    word(10, 54);
    word(14, 40);
    word(18, width);
    word(22, 0u - height);
    word(26, 1, 2);
    word(28, 32, 2);
    std::copy(bgra.begin(), bgra.end(), bytes.begin() + 54);
    return bytes;
}
} // namespace Art

inline void PurgeLegacyArt(const fs::path& local_state) {
    static std::mutex mutex;
    const std::lock_guard<std::mutex> lock(mutex);
    const auto folder = local_state / "library";
    const auto marker = folder / "own-art-v1";
    std::error_code ec;
    if (fs::exists(marker, ec))
        return;
    fs::create_directories(folder, ec);
    bool complete = !ec;
    fs::directory_iterator it(folder, ec), end;
    if (ec)
        return;
    while (it != end) {
        const auto file = it->path();
        const auto name = file.filename().string();
        // Old index and all old JPEGs (including store icons), plus interrupted banner writes.
        if ((file.extension() == ".jpg" && file.stem().string().size() == 16) ||
            (name.rfind("eshop", 0) == 0) || name.find(".banner.") != std::string::npos ||
            (name.size() == 28 && name.substr(16) == ".jpg.partial")) {
            fs::remove(file, ec);
            complete = complete && !ec;
        }
        it.increment(ec);
        if (ec)
            return;
    }
    if (complete)
        Art::WriteBytes(marker, {1});
}

} // namespace EdenXbox::Ui
