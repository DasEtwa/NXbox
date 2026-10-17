// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include "core/hle/service/mii/types/store_data.h"

namespace EdenXbox::Moru {
// A deliberately small original parts catalogue. Values are public service enum IDs,
// not bit widths. Edits touch only the selected field, preserving imported parts and IDs.
enum class Field {
    Face,
    Skin,
    Hair,
    HairColor,
    Brows,
    Eyes,
    EyeColor,
    Nose,
    Mouth,
    Beard,
    Mustache,
    Glasses,
    GlassesColor,
    Favorite,
    Gender,
    Height,
    Build,
    Count
};
inline constexpr int FieldCount = static_cast<int>(Field::Count);
inline std::span<const int> Options(Field field) {
    using namespace Service::Mii;
    static constexpr int face[] = {int(FacelineType::Sharp), int(FacelineType::Rounded),
                                   int(FacelineType::Large), int(FacelineType::Angular)};
    static constexpr int skin[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static constexpr int hair[] = {
        int(HairType::NormalShort), int(HairType::PartingShort),
        int(HairType::Afro),        int(HairType::FrontLock),
        int(HairType::NormalLong),  int(HairType::PeaksTop),
        int(HairType::PartingLong), int(HairType::TwoShortSidedPonyTails)};
    static constexpr int hairColor[] = {8, 1, 2, 3, 4, 5, 6, 7};
    static constexpr int brows[] = {int(EyebrowType::Flat), int(EyebrowType::LowArchMedium),
                                    int(EyebrowType::HighArchFlat), int(EyebrowType::None)};
    static constexpr int eyes[] = {int(EyeType::Normal), int(EyeType::OvalWhite),
                                   int(EyeType::RoundedWhite), int(EyeType::NormalTopLine)};
    static constexpr int eyeColor[] = {8, 9, 10, 11, 12, 13};
    static constexpr int nose[] = {int(NoseType::Dot), int(NoseType::Button),
                                   int(NoseType::Rounded), int(NoseType::Potato)};
    static constexpr int mouth[] = {int(MouthType::Neutral), int(MouthType::NeutralStroke),
                                    int(MouthType::SmileTeeth), int(MouthType::TeethRounded)};
    static constexpr int beard[] = {int(BeardType::None), int(BeardType::Goatee),
                                    int(BeardType::Full)};
    static constexpr int mustache[] = {int(MustacheType::None), int(MustacheType::Pencil),
                                       int(MustacheType::Normal)};
    static constexpr int glasses[] = {int(GlassType::None), int(GlassType::Oval),
                                      int(GlassType::Wayfarer), int(GlassType::Rectangle)};
    static constexpr int glassesColor[] = {8, 14, 15, 16, 17, 18, 0};
    static constexpr int favorite[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static constexpr int gender[] = {0, 1};
    static constexpr auto slider = [] {
        std::array<int, 128> values{};
        for (int i = 0; i < 128; ++i)
            values[i] = i;
        return values;
    }();
    switch (field) {
    case Field::Face:
        return face;
    case Field::Skin:
        return skin;
    case Field::Hair:
        return hair;
    case Field::HairColor:
        return hairColor;
    case Field::Brows:
        return brows;
    case Field::Eyes:
        return eyes;
    case Field::EyeColor:
        return eyeColor;
    case Field::Nose:
        return nose;
    case Field::Mouth:
        return mouth;
    case Field::Beard:
        return beard;
    case Field::Mustache:
        return mustache;
    case Field::Glasses:
        return glasses;
    case Field::GlassesColor:
        return glassesColor;
    case Field::Favorite:
        return favorite;
    case Field::Gender:
        return gender;
    case Field::Height:
        return slider;
    case Field::Build:
        return slider;
    default:
        return {};
    }
}
inline int Value(const Service::Mii::StoreData& data, Field field) {
    switch (field) {
    case Field::Face:
        return static_cast<int>(data.GetFacelineType());
    case Field::Skin:
        return static_cast<int>(data.GetFacelineColor());
    case Field::Hair:
        return static_cast<int>(data.GetHairType());
    case Field::HairColor:
        return static_cast<int>(data.GetHairColor());
    case Field::Brows:
        return static_cast<int>(data.GetEyebrowType());
    case Field::Eyes:
        return static_cast<int>(data.GetEyeType());
    case Field::EyeColor:
        return static_cast<int>(data.GetEyeColor());
    case Field::Nose:
        return static_cast<int>(data.GetNoseType());
    case Field::Mouth:
        return static_cast<int>(data.GetMouthType());
    case Field::Beard:
        return static_cast<int>(data.GetBeardType());
    case Field::Mustache:
        return static_cast<int>(data.GetMustacheType());
    case Field::Glasses:
        return static_cast<int>(data.GetGlassType());
    case Field::GlassesColor:
        return static_cast<int>(data.GetGlassColor());
    case Field::Favorite:
        return static_cast<int>(data.GetFavoriteColor());
    case Field::Gender:
        return static_cast<int>(data.GetGender());
    case Field::Height:
        return static_cast<int>(data.GetHeight());
    case Field::Build:
        return static_cast<int>(data.GetBuild());
    default:
        return 0;
    }
}
inline void Choose(Service::Mii::StoreData& data, Field field, int choice) {
    using namespace Service::Mii;
    const auto options = Options(field);
    if (options.empty())
        return;
    const int value = options[std::clamp(choice, 0, static_cast<int>(options.size()) - 1)];
    switch (field) {
    case Field::Face:
        data.SetFacelineType(static_cast<FacelineType>(value));
        break;
    case Field::Skin:
        data.SetFacelineColor(static_cast<FacelineColor>(value));
        break;
    case Field::Hair:
        data.SetHairType(static_cast<HairType>(value));
        break;
    case Field::HairColor:
        data.SetHairColor(static_cast<CommonColor>(value));
        break;
    case Field::Brows:
        data.SetEyebrowType(static_cast<EyebrowType>(value));
        break;
    case Field::Eyes:
        data.SetEyeType(static_cast<EyeType>(value));
        break;
    case Field::EyeColor:
        data.SetEyeColor(static_cast<CommonColor>(value));
        break;
    case Field::Nose:
        data.SetNoseType(static_cast<NoseType>(value));
        break;
    case Field::Mouth:
        data.SetMouthType(static_cast<MouthType>(value));
        break;
    case Field::Beard:
        data.SetBeardType(static_cast<BeardType>(value));
        break;
    case Field::Mustache:
        data.SetMustacheType(static_cast<MustacheType>(value));
        break;
    case Field::Glasses:
        data.SetGlassType(static_cast<GlassType>(value));
        break;
    case Field::GlassesColor:
        data.SetGlassColor(static_cast<CommonColor>(value));
        break;
    case Field::Favorite:
        data.SetFavoriteColor(static_cast<FavoriteColor>(value));
        break;
    case Field::Gender:
        data.SetGender(static_cast<Gender>(value));
        break;
    case Field::Height:
        data.SetHeight(static_cast<u8>(value));
        break;
    case Field::Build:
        data.SetBuild(static_cast<u8>(value));
        break;
    default:
        break;
    }
    if (field == Field::HairColor) {
        data.SetEyebrowColor(data.GetHairColor());
        data.SetBeardColor(data.GetHairColor());
    }
    data.SetChecksum();
}
// -1 represents an imported part outside this editor's catalogue. Leave it intact
// until the user changes that category.
inline int Choice(const Service::Mii::StoreData& data, Field field) {
    const auto options = Options(field);
    const auto found = std::find(options.begin(), options.end(), Value(data, field));
    return found == options.end() ? -1 : static_cast<int>(found - options.begin());
}
inline bool Rename(Service::Mii::StoreData& data, std::u16string_view text) {
    using namespace Service::Mii;
    if (text.empty() || text.size() > MaxNameSize)
        return false;
    // UTF-16 storage: reject embedded controls and unpaired surrogate halves.
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || (c >= 0xDC00 && c <= 0xDFFF))
            return false;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (++i == text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF)
                return false;
        }
    }
    Nickname name{};
    std::copy(text.begin(), text.end(), name.data.begin());
    data.SetNickname(name);
    data.SetChecksum();
    return true;
}
} // namespace EdenXbox::Moru
