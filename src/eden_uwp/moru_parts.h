// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include "eden_uwp/moru_model.h"

namespace EdenXbox::Moru {
// Original, deliberately abstract artwork; no console shapes or textures.
// RGB approximations in legacy order. raw_data.cpp's Ver3*ColorTable maps
// catalogue positions to stored CommonColor IDs (notably legacy black -> 8).
// Hair: black, brown, red, light brown, grey, olive, auburn, blonde.
inline constexpr std::uint32_t Ink = 0x252127;
inline constexpr std::uint32_t Paper = 0xFFF9F0;
inline constexpr std::uint32_t Lips = 0xA45358;
// The first six skin IDs follow the legacy conversion, including dark brown at
// ID 5, despite FacelineColor's Porcelain name. IDs 6-9 are extended estimates.
inline constexpr std::uint32_t Skin[] = {0xFFD3AD, 0xFFB66B, 0xDE7942, 0xFFAA8C, 0xAD5129,
                                         0x632C18, 0xF2DCC5, 0xE9C8AD, 0xB8805A, 0x704936};
inline constexpr std::uint32_t Hair[] = {0x1E1A18, 0x402015, 0x5C1810, 0x7C3A14,
                                         0x787880, 0x4E3E0E, 0x582410, 0xD09E50};
// Eyes: black, grey, brown, hazel, blue, green (stored IDs 8-13).
inline constexpr std::uint32_t Eyes[] = {0x000000, 0x6C7070, 0x663C2C,
                                         0x605E30, 0x4654A8, 0x387058};
// Glasses: black, brown, red, blue, gold, grey, extended white.
// Stored IDs: 8, 14, 15, 16, 17, 18, 0. White is not legacy black ID 8.
inline constexpr std::uint32_t Glasses[] = {0x181818, 0x603810, 0xA81008, 0x203068,
                                            0xA86000, 0x787068, 0xFFFFFF};
// FavoriteColor enum order: red, orange, yellow, lime, green, blue, light blue,
// pink, purple, brown, white, black.
inline constexpr std::uint32_t Favorites[] = {0xD21E14, 0xFF6E19, 0xFFD820, 0x78D220,
                                              0x007830, 0x0A48B4, 0x3CAADE, 0xF55A7D,
                                              0x7328AD, 0x483818, 0xE0E0E0, 0x181814};

struct Part {
    float x, y, width, height, radius;
    std::uint32_t color;
};
using Scene = std::vector<Part>;
inline std::uint32_t PartColor(const Service::Mii::StoreData& data, Field field,
                               std::span<const std::uint32_t> palette) {
    const int choice = Choice(data, field);
    return palette[choice < 0 ? 0 : static_cast<std::size_t>(choice) % palette.size()];
}
inline std::uint32_t HairColor(Service::Mii::CommonColor color) {
    const auto ids = Options(Field::HairColor);
    const auto found = std::find(ids.begin(), ids.end(), static_cast<int>(color));
    return Hair[found == ids.end() ? 0 : static_cast<std::size_t>(found - ids.begin())];
}
// One shared 256-unit scene drives the Direct2D live preview and the profile icon.
inline Scene MakeScene(const Service::Mii::StoreData& data) {
    Scene parts;
    const auto add = [&](float x, float y, float w, float h, float radius, std::uint32_t color) {
        parts.push_back({x, y, w, h, std::min(radius, std::min(w, h) / 2), color});
    };
    const auto skin = Skin[Value(data, Field::Skin) % std::size(Skin)];
    const auto hair = HairColor(data.GetHairColor());
    const auto eyebrow_color = HairColor(data.GetEyebrowColor());
    const auto beard_color = HairColor(data.GetBeardColor());
    const auto eye = PartColor(data, Field::EyeColor, Eyes);
    const auto glass = PartColor(data, Field::GlassesColor, Glasses);
    const float body_width = 76 + data.GetBuild() * 0.35f;
    const float shoulder_y = 210 - data.GetHeight() * 0.09f;
    add(128 - body_width / 2, shoulder_y, body_width, 256 - shoulder_y, 28,
        Favorites[Value(data, Field::Favorite) % std::size(Favorites)]);
    if (data.GetGender() == Service::Mii::Gender::Female)
        add(118, shoulder_y + 5, 20, 14, 7, Paper);
    const int style = std::max(0, Choice(data, Field::Hair));
    // Back hair is behind the face and ears; long styles reach the shoulders.
    if (style == 4 || style == 6)
        add(58, 45, 140, 165, 42, hair);
    if (style == 2)
        add(49, 25, 158, 139, 65, hair);
    if (style == 7) {
        add(43, 77, 32, 103, 16, hair);
        add(181, 77, 32, 103, 16, hair);
    }
    add(114, 178, 28, 36, 10, skin);
    const int face = std::max(0, Choice(data, Field::Face));
    const float face_width = face == 2 ? 126.0f : face == 3 ? 104.0f : 114.0f;
    const float face_radius = face == 0 ? 46.0f : face == 1 ? 56.0f : face == 2 ? 38.0f : 26.0f;
    add(128 - face_width / 2, 57, face_width, 140, face_radius, skin);
    add(61, 116, 18, 30, 9, skin);
    add(177, 116, 18, 30, 9, skin);
    // Catalogue IDs 1, 8, 39, 7, 0, 14, 6, 129: short, parted short,
    // afro, front lock, long, peaked top, parted long, two side pony tails.
    if (style == 0)
        add(69, 43, 118, 33, 22, hair);
    if (style == 1 || style == 6) {
        // Two unequal lobes leave a visible part instead of a solid fringe.
        add(67, 43, 76, 45, 22, hair);
        add(148, 45, 41, 37, 18, hair);
        if (style == 1)
            add(70, 64, 58, 33, 14, hair);
        else {
            add(60, 69, 24, 133, 11, hair);
            add(172, 69, 22, 133, 11, hair);
            add(76, 65, 55, 31, 15, hair);
        }
    }
    if (style == 2) {
        for (int i = 0; i < 5; ++i)
            add(52 + i * 29.0f, 29 - (i % 2) * 9.0f, 36, 49, 18, hair);
    }
    if (style == 3) {
        add(69, 43, 118, 34, 17, hair);
        add(103, 60, 38, 43, 16, hair);
    }
    if (style == 4) {
        add(66, 43, 124, 41, 20, hair);
        add(61, 70, 23, 138, 11, hair);
        add(172, 70, 23, 138, 11, hair);
    }
    if (style == 5) {
        add(69, 46, 118, 31, 15, hair);
        add(88, 33, 27, 35, 12, hair);
        add(114, 18, 28, 51, 12, hair);
        add(141, 33, 27, 35, 12, hair);
    }
    if (style == 7) {
        add(69, 43, 118, 41, 20, hair);
        add(54, 69, 27, 30, 13, hair);
        add(175, 69, 27, 30, 13, hair);
    }
    const int brows = std::max(0, Choice(data, Field::Brows));
    if (brows != 3) {
        for (float x : {87.0f, 145.0f}) {
            add(x, 105 - brows * 2.0f, 25, 4 + brows * 2.0f, brows == 1 ? 1 : 4, eyebrow_color);
            if (brows == 2)
                add(x + 3, 99, 18, 4, 2, eyebrow_color);
        }
    }
    const int eyes = std::max(0, Choice(data, Field::Eyes));
    for (float x : {88.0f, 145.0f}) {
        add(x, 119, 25, 15 + eyes * 2.0f, eyes == 2 ? 3 : 12, Paper);
        add(x + 8, 120, 10, 13 + eyes * 2.0f, 5, eye);
        add(x + 10, 122, 3, 4, 1.5f, Paper);
        if (eyes == 3)
            add(x - 2, 116, 28, 3, 1, Ink);
    }
    const int nose = std::max(0, Choice(data, Field::Nose));
    add(122 - nose, 138, 12 + nose * 2.0f, 8 + nose * 3.0f, 3, Skin[4]);
    const int beard = std::max(0, Choice(data, Field::Beard));
    if (beard == 1)
        add(115, 178, 26, 23, 10, beard_color);
    if (beard == 2)
        add(83, 165, 90, 34, 17, beard_color);
    const int mouth = std::max(0, Choice(data, Field::Mouth));
    add(108, 169, 40, 6 + mouth * 3.0f, mouth == 1 ? 2 : 9, Lips);
    if (mouth >= 2)
        add(113, 169, 30, 5, 2, Paper);
    const int mustache = std::max(0, Choice(data, Field::Mustache));
    if (mustache > 0) {
        add(103, 158, 25, mustache == 1 ? 4.0f : 9.0f, 4, beard_color);
        add(128, 158, 25, mustache == 1 ? 4.0f : 9.0f, 4, beard_color);
    }
    const int glasses = std::max(0, Choice(data, Field::Glasses));
    if (glasses > 0) {
        // Four narrow sides form an open lens; eyes remain visible through it.
        const float r = glasses == 1 ? 2.0f : 1.0f;
        const float lens_height = glasses == 1 ? 29.0f : glasses == 2 ? 25.0f : 21.0f;
        for (float x : {80.0f, 140.0f}) {
            add(x, 114, 36, 4, r, glass);
            add(x, 114 + lens_height - 4, 36, 4, r, glass);
            add(x, 114, 4, lens_height, r, glass);
            add(x + 32, 114, 4, lens_height, r, glass);
        }
        add(116, 122, 24, 4, 2, glass);
    }
    return parts;
}
inline bool Contains(const Part& part, float x, float y) {
    const float cx = std::clamp(x, part.x + part.radius, part.x + part.width - part.radius);
    const float cy = std::clamp(y, part.y + part.radius, part.y + part.height - part.radius);
    return x >= part.x && x <= part.x + part.width && y >= part.y && y <= part.y + part.height &&
           (x - cx) * (x - cx) + (y - cy) * (y - cy) <= part.radius * part.radius;
}
// Supersampled premultiplied BGRA, ready for the existing avatar composite pipeline.
inline std::vector<std::uint8_t> Rasterize(const Scene& scene) {
    std::vector<std::uint8_t> pixels(256 * 256 * 4);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            unsigned channels[4]{};
            for (float sy : {0.25f, 0.75f}) {
                for (float sx : {0.25f, 0.75f}) {
                    for (auto it = scene.rbegin(); it != scene.rend(); ++it) {
                        if (!Contains(*it, x + sx, y + sy))
                            continue;
                        channels[0] += it->color & 255;
                        channels[1] += (it->color >> 8) & 255;
                        channels[2] += (it->color >> 16) & 255;
                        channels[3] += 255;
                        break;
                    }
                }
            }
            for (int c = 0; c < 4; ++c)
                pixels[(y * 256 + x) * 4 + c] = (channels[c] + 2) / 4;
        }
    }
    return pixels;
}
} // namespace EdenXbox::Moru
