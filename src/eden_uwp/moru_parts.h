// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include "eden_uwp/moru_model.h"

namespace EdenXbox::Moru {
// Original, deliberately abstract artwork. These are Moru's art tokens, separate
// from the interface theme. RGB approximations are not console rendering tables.
inline constexpr std::uint32_t Ink = 0x252127;
inline constexpr std::uint32_t Paper = 0xFFF9F0;
inline constexpr std::uint32_t Lips = 0xA45358;
inline constexpr std::uint32_t Skin[] = {0xF4CEAE, 0xE8B995, 0xDDA581, 0xC88D60, 0xA56D4F,
                                         0xFAE3CF, 0xF2DCC5, 0xE9C8AD, 0xB8805A, 0x704936};
inline constexpr std::uint32_t Hair[] = {0x29252A, 0x392A26, 0x704839, 0xA2784D,
                                         0x927251, 0x7A6350, 0xBFA45A, 0xD0C6B9};
inline constexpr std::uint32_t Eyes[] = {Ink, 0x604334, 0x727D50, 0x416D8C, 0x777582, 0x42454B};
inline constexpr std::uint32_t Glasses[] = {Ink,      0x775144, 0xA94D4C, 0x476C89,
                                            0x8A7292, 0xB59B58, Paper};
inline constexpr std::uint32_t Favorites[] = {0xCE4846, 0xE68842, 0xE9C454, 0xA2BE53,
                                              0x4D9871, 0x456FAE, 0x70BED4, 0xD57FA7,
                                              0x8E72AF, 0x815D48, 0xEDEBE5, 0x34333A};

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
// One shared 256-unit scene drives the Direct2D live preview and the profile icon.
inline Scene MakeScene(const Service::Mii::StoreData& data) {
    Scene parts;
    const auto add = [&](float x, float y, float w, float h, float radius, std::uint32_t color) {
        parts.push_back({x, y, w, h, std::min(radius, std::min(w, h) / 2), color});
    };
    const auto skin = Skin[Value(data, Field::Skin) % std::size(Skin)];
    const auto hair = PartColor(data, Field::HairColor, Hair);
    const auto eye = PartColor(data, Field::EyeColor, Eyes);
    const auto glass = PartColor(data, Field::GlassesColor, Glasses);
    const float body_width = 76 + data.GetBuild() * 0.35f;
    const float shoulder_y = 210 - data.GetHeight() * 0.09f;
    add(128 - body_width / 2, shoulder_y, body_width, 256 - shoulder_y, 28,
        Favorites[Value(data, Field::Favorite) % std::size(Favorites)]);
    if (data.GetGender() == Service::Mii::Gender::Female)
        add(118, shoulder_y + 5, 20, 14, 7, Paper);
    add(114, 178, 28, 36, 10, skin);
    const int face = std::max(0, Choice(data, Field::Face));
    const float face_width = face == 2 ? 126.0f : face == 3 ? 104.0f : 114.0f;
    const float face_radius = face == 0 ? 46.0f : face == 1 ? 56.0f : face == 2 ? 38.0f : 26.0f;
    add(128 - face_width / 2, 57, face_width, 140, face_radius, skin);
    add(61, 116, 18, 30, 9, skin);
    add(177, 116, 18, 30, 9, skin);
    const int style = std::max(0, Choice(data, Field::Hair));
    // Eight distinct silhouettes: crop, sweep, curls, fringe, bob, crest, side-part, buns.
    add(69, 43, 118, style == 0 ? 33.0f : 46.0f, 22, hair);
    if (style == 1)
        add(72, 64, 64, 36, 14, hair);
    if (style == 6)
        add(126, 64, 56, 36, 14, hair);
    if (style == 2) {
        for (int i = 0; i < 5; ++i)
            add(63 + i * 24.0f, 40 - (i % 2) * 8.0f, 34, 38, 17, hair);
    }
    if (style == 3) {
        for (int i = 0; i < 4; ++i)
            add(75 + i * 25.0f, 67, 23, 32 - i * 3.0f, 8, hair);
    }
    if (style == 4) {
        add(65, 70, 18, 100, 9, hair);
        add(173, 70, 18, 100, 9, hair);
    }
    if (style == 5)
        add(110, 22, 36, 52, 12, hair);
    if (style == 7) {
        add(49, 49, 36, 38, 18, hair);
        add(171, 49, 36, 38, 18, hair);
    }
    const int brows = std::max(0, Choice(data, Field::Brows));
    if (brows != 3) {
        for (float x : {87.0f, 145.0f}) {
            add(x, 105 - brows * 2.0f, 25, 4 + brows * 2.0f, brows == 1 ? 1 : 4, hair);
            if (brows == 2)
                add(x + 3, 99, 18, 4, 2, hair);
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
        add(115, 178, 26, 23, 10, hair);
    if (beard == 2)
        add(83, 165, 90, 34, 17, hair);
    const int mouth = std::max(0, Choice(data, Field::Mouth));
    add(108, 169, 40, 6 + mouth * 3.0f, mouth == 1 ? 2 : 9, Lips);
    if (mouth >= 2)
        add(113, 169, 30, 5, 2, Paper);
    const int mustache = std::max(0, Choice(data, Field::Mustache));
    if (mustache > 0) {
        add(103, 158, 25, mustache == 1 ? 4.0f : 9.0f, 4, hair);
        add(128, 158, 25, mustache == 1 ? 4.0f : 9.0f, 4, hair);
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
