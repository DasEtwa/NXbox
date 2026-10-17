// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include "common/fs/path_util.h"
#include "core/hle/service/mii/mii_database_manager.h"
#include "core/hle/service/mii/mii_util.h"
#include "core/hle/service/mii/types/char_info.h"
#include "eden_uwp/moru_parts.h"
#include "eden_uwp/ui/avatar_composite.h"

// Only host infrastructure is substituted. Character packing, CRC, validation,
// database operations and IOFile are compiled from the production sources.
void AssertFailSoftImpl() {
    std::abort();
}
[[noreturn]] void AssertFatalImpl() {
    std::abort();
}
namespace Common::FS {
std::filesystem::path test_nand;
const std::filesystem::path& GetEdenPath(EdenPath) {
    return test_nand;
}
bool Exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}
bool IsFile(const std::filesystem::path& path) {
    return std::filesystem::is_regular_file(path);
}
bool CreateDirs(const std::filesystem::path& path) {
    return std::filesystem::create_directories(path) || std::filesystem::is_directory(path);
}
bool RemoveFile(const std::filesystem::path& path) {
    return std::filesystem::remove(path);
}
u64 GetSize(const std::filesystem::path& path) {
    return std::filesystem::file_size(path);
}
bool NewFile(const std::filesystem::path& path, u64 size) {
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.close();
    std::filesystem::resize_file(path, size);
    return true;
}
} // namespace Common::FS

using namespace Service::Mii;
namespace Moru = EdenXbox::Moru;
void Valid(const StoreData& data) {
    assert(data.IsValid() == ValidationResult::NoErrors);
    CharInfo info;
    info.SetFromStoreData(data);
    assert(info.Verify() == ValidationResult::NoErrors);
    assert(info.GetCreateId() == data.GetCreateId());
}
void TestLegacyHairColors() {
    StoreData data;
    data.BuildDefault(0);
    const auto rgb = [&](int id) {
        data.SetHairColor(static_cast<CommonColor>(id));
        const auto color = Moru::HairColor(data.GetHairColor());
        // Also exercise the catalogue lookup used by the editor's color choices.
        assert(color == Moru::PartColor(data, Moru::Field::HairColor, Moru::Hair));
        return std::array<float, 3>{((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                                    (color & 255) / 255.0f};
    };
    const auto blonde = rgb(7);
    const auto grey = rgb(4);
    assert(blonde[0] > blonde[1] && blonde[1] > blonde[2] && blonde[0] > 0.6f &&
           std::abs(grey[0] - grey[1]) < 0.05f && std::abs(grey[1] - grey[2]) < 0.05f);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    TestLegacyHairColors();
    Common::FS::test_nand = argv[1];
    StoreData original;
    original.BuildDefault(0);
    Valid(original);
    assert((original.GetCreateId().uuid[6] & 0xF0) == 0x40);
    assert((original.GetCreateId().uuid[8] & 0xC0) == 0x80);
    for (int f = 0; f < Moru::FieldCount; ++f) {
        const auto field = static_cast<Moru::Field>(f);
        const auto options = Moru::Options(field);
        assert(!options.empty());
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            auto data = original;
            Moru::Choose(data, field, i);
            Valid(data);
            assert(Moru::Choice(data, field) == i);
            assert(Moru::Value(data, field) == options[i]);
            assert(data.GetCreateId() == original.GetCreateId());
            CharInfo info;
            info.SetFromStoreData(data);
            StoreData via_info;
            via_info.BuildWithCharInfo(info);
            Valid(via_info);
            assert(Moru::Value(via_info, field) == options[i]);
            // Round trip the real packed representation, not a duplicate model.
            std::array<std::byte, sizeof(StoreData)> bytes;
            std::memcpy(bytes.data(), &data, bytes.size());
            StoreData restored;
            std::memcpy(&restored, bytes.data(), bytes.size());
            Valid(restored);
            assert(Moru::Value(restored, field) == options[i]);
        }
        auto data = original;
        Moru::Choose(data, field, -999);
        assert(Moru::Choice(data, field) == 0);
        Moru::Choose(data, field, 999);
        assert(Moru::Choice(data, field) == static_cast<int>(options.size()) - 1);
        Valid(data);
    }
    auto imported = original;
    imported.SetHairType(HairType::Max);
    imported.SetHairFlip(HairFlip::Right);
    imported.SetEyeRotate(MaxEyeRotate);
    imported.SetChecksum();
    assert(Moru::Choice(imported, Moru::Field::Hair) == -1);
    Moru::Choose(imported, Moru::Field::Height, 127);
    assert(imported.GetHairType() == HairType::Max);
    assert(imported.GetHairFlip() == HairFlip::Right);
    assert(imported.GetEyeRotate() == MaxEyeRotate);
    Valid(imported);
    assert(Moru::Rename(imported, u"João"));
    assert(Moru::Rename(imported, u"1234567890"));
    auto before = imported;
    assert(!Moru::Rename(imported, u""));
    assert(!Moru::Rename(imported, u"12345678901"));
    assert(!Moru::Rename(imported, std::u16string_view(u"a\0b", 3)));
    const char16_t unpaired[] = {0xD800};
    assert(!Moru::Rename(imported, {unpaired, 1}));
    assert(std::memcmp(&before, &imported, sizeof(before)) == 0);
    assert(Moru::Rename(imported, u"Moru \U0001F600"));
    Valid(imported);
    auto damaged = imported;
    damaged.SetHeight(0); // Deliberately do not refresh the checksum.
    assert(damaged.IsValid() == ValidationResult::InvalidChecksum);
    const char crc_fixture[] = "123456789";
    assert(MiiUtil::CalculateCrc16(crc_fixture, 9) == Common::swap16(u16{0x31C3}));

    DatabaseManager manager;
    DatabaseSessionMetadata metadata{};
    bool broken = false;
    assert(manager.MountSaveData().IsSuccess());
    assert(manager.Initialize(metadata, broken).IsSuccess() && !broken);
    assert(manager.GetCount(metadata) == 0);
    assert(manager.AddOrReplace(metadata, damaged).IsError());
    StoreData last;
    for (std::size_t i = 0; i < MaxDatabaseLength; ++i) {
        last.BuildDefault(0);
        assert(manager.AddOrReplace(metadata, last).IsSuccess());
    }
    assert(manager.IsFullDatabase());
    assert(manager.GetCount(metadata) == MaxDatabaseLength);
    StoreData excess;
    excess.BuildDefault(0);
    assert(manager.AddOrReplace(metadata, excess).IsError());
    assert(manager.GetCount(metadata) == MaxDatabaseLength);
    assert(manager.SaveDatabase().IsSuccess());
    const auto file = Common::FS::test_nand / "system/save/8000000000000030/MiiDatabase.dat";
    assert(std::filesystem::file_size(file) == sizeof(NintendoFigurineDatabase));
    DatabaseManager reloaded;
    DatabaseSessionMetadata session{};
    assert(reloaded.MountSaveData().IsSuccess());
    assert(reloaded.Initialize(session, broken).IsSuccess() && !broken);
    assert(reloaded.IsFullDatabase());
    StoreData restored;
    reloaded.Get(restored, MaxDatabaseLength - 1, session);
    Valid(restored);
    assert(restored.GetCreateId() == last.GetCreateId());
    assert(Moru::Rename(restored, u"Updated"));
    assert(reloaded.AddOrReplace(session, restored).IsSuccess()); // Replace at capacity.
    assert(reloaded.GetCount(session) == MaxDatabaseLength);
    assert(reloaded.Delete(session, restored.GetCreateId()).IsSuccess());
    assert(reloaded.GetCount(session) == MaxDatabaseLength - 1);
    assert(reloaded.AddOrReplace(session, excess).IsSuccess());
    assert(reloaded.SaveDatabase().IsSuccess());
    {
        NintendoFigurineDatabase raw;
        std::ifstream in(file, std::ios::binary);
        in.read(reinterpret_cast<char*>(&raw), sizeof(raw));
        assert(in.good());
        assert(raw.CheckIntegrity().IsSuccess());
        raw.CorruptCrc();
        assert(raw.CheckIntegrity().IsError());
    }
    // Art and icon pipeline: transparent premultiplied edges, opaque centres,
    // and a different raster for each catalogue choice.
    for (int f = 0; f < Moru::FieldCount; ++f) {
        const auto field = static_cast<Moru::Field>(f);
        if (field == Moru::Field::Height || field == Moru::Field::Build)
            continue;
        std::set<std::vector<u8>> unique;
        for (int i = 0; i < static_cast<int>(Moru::Options(field).size()); ++i) {
            auto data = original;
            data.SetGlassType(GlassType::Oval); // Colour choices need a visible frame.
            Moru::Choose(data, field, i);
            auto pixels = Moru::Rasterize(Moru::MakeScene(data));
            assert(pixels.size() == 256 * 256 * 4);
            assert(pixels[3] == 0);
            assert(pixels[(128 * 256 + 128) * 4 + 3] == 255);
            for (std::size_t p = 0; p < pixels.size(); p += 4)
                for (int c = 0; c < 3; ++c)
                    assert(pixels[p + c] <= pixels[p + 3]);
            unique.insert(std::move(pixels));
        }
        if (unique.size() != Moru::Options(field).size()) {
            std::fprintf(stderr, "Field %d has %zu unique rasters for %zu choices\n", f,
                         unique.size(), Moru::Options(field).size());
            std::abort();
        }
    }
    auto pixels = Moru::Rasterize(Moru::MakeScene(original));
    for (std::size_t p = 0; p < pixels.size(); p += 4) {
        for (int c = 0; c < 3; ++c)
            pixels[p + c] = EdenXbox::Ui::CompositeAvatarChannel(pixels[p + c], pixels[p + 3], 200);
        pixels[p + 3] = 255;
    }
    assert(pixels[0] == 200 && pixels[1] == 200 && pixels[2] == 200 && pixels[3] == 255);
}
