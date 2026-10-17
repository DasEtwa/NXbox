// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include "eden_uwp/ui/art_cache.h"
#include "eden_uwp/ui/tinfoil_index.h"

namespace EdenXbox::Ui {
struct Pixels;
bool CacheBitmap(const std::filesystem::path& file, const Pixels& pixels);

enum class ArtState { Pending, Ready, None };
// Persistent keyless community hero cache, independent of authenticated shop sessions.
class BackgroundArtSource {
public:
    explicit BackgroundArtSource(std::filesystem::path local_state);
    ~BackgroundArtSource();
    BackgroundArtSource(const BackgroundArtSource&) = delete;
    BackgroundArtSource& operator=(const BackgroundArtSource&) = delete;
    void Request(const std::string& title_id, const std::string& name, const std::string& english);
    ArtState Get(const std::string& title_id, std::filesystem::path& file) const;
    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};
// Shop-provided, same-origin artwork remains isolated from community backgrounds.
class ShopArtSource {
public:
    explicit ShopArtSource(std::filesystem::path local_state);
    ~ShopArtSource();
    ShopArtSource(const ShopArtSource&) = delete;
    ShopArtSource& operator=(const ShopArtSource&) = delete;
    void Request(const std::string& url, const std::string& origin,
                 const Tinfoil::Headers& headers);
    ArtState Get(const std::string& url, std::filesystem::path& file) const;
    void Discard(const std::string& url);
    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};
} // namespace EdenXbox::Ui
