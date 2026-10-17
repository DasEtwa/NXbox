// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
namespace EdenXbox::SaveSync::Policy {
constexpr std::size_t GameNameBuffer = 0x201;
constexpr std::size_t OwnerNameBuffer = 0x41;
constexpr std::size_t FolderPathBuffer = 0x220;
inline std::string SanitizeName(const std::string& in, std::size_t outsz) {
    if (outsz == 0) {
        return std::string();
    }
    std::string out;
    for (std::size_t n = 0; n < in.size() && n + 1 < outsz; ++n) {
        const unsigned char c = static_cast<unsigned char>(in[n]);
        if (c == 0) {
            break;
        }
        const bool bad = c < 0x20 || std::strchr("/\\:*?\"<>|", static_cast<int>(c)) != nullptr;
        out.push_back(bad ? '_' : static_cast<char>(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) {
        out = "sem-nome";
        if (out.size() > outsz - 1) {
            out.resize(outsz - 1); // snprintf(out, outsz, ...) cuts to the buffer
        }
    }
    return out;
}

// "<game>/<account>", always two levels. The account part is what separates one save from another,
// so it is never the one that gets truncated.
inline std::string BuildCloudFolderPath(const std::string& game_name,
                                        const std::string& owner_name) {
    const std::string game = SanitizeName(game_name, GameNameBuffer);
    const std::string owner = SanitizeName(owner_name, OwnerNameBuffer);
    const std::size_t reserved = owner.size() + 2;
    const std::size_t room = FolderPathBuffer > reserved ? FolderPathBuffer - reserved : 0;
    return game.substr(0, std::min(game.size(), room)) + "/" + owner;
}

enum class SyncAction { Nothing, Upload, Download, Equal, Conflict };

// Three numbers decide it: the Xbox save now, the cloud save now, and the save as it was when the
// two last matched (the marker). With only two numbers you can see they differ, not who moved.
inline SyncAction Decide(bool local_empty, bool cloud_empty, std::uint64_t local_fp,
                         std::uint64_t cloud_fp, bool has_marker, std::uint64_t marker) {
    if (local_empty && cloud_empty) {
        return SyncAction::Nothing;
    }
    if (cloud_empty) {
        return SyncAction::Upload; // first sync of this game
    }
    if (local_empty) {
        return SyncAction::Download; // nothing here to lose
    }
    if (local_fp == cloud_fp) {
        return SyncAction::Equal;
    }
    if (!has_marker) {
        return SyncAction::Conflict; // both sides have a save and no earlier sync says who moved
    }
    const bool local_changed = local_fp != marker;
    const bool cloud_changed = cloud_fp != marker;
    if (local_changed && cloud_changed) {
        return SyncAction::Conflict;
    }
    if (cloud_changed) {
        return SyncAction::Download;
    }
    return SyncAction::Upload;
}

enum class SaveKind { Account, Device, System };
inline std::string Hex16(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value));
    return buffer;
}
inline std::string NacpGameName(const std::vector<std::string>& names, std::uint64_t title_id = 0) {
    for (std::size_t i = 0; i < std::min<std::size_t>(16, names.size()); ++i) {
        if (!names[i].empty())
            return names[i].substr(0, 0x200);
    }
    // This literal is SwitchSaveSync's persisted folder name, independent of UI language.
    return title_id ? "Titulo desconhecido (" + Hex16(title_id) + ")" : std::string{};
}
inline std::filesystem::path EdenSavePath(const std::filesystem::path& nand, SaveKind kind,
                                          std::uint64_t title, const std::string& user,
                                          const std::string& raw_uuid,
                                          std::uint64_t system_save_id = 0,
                                          bool system_space = false) {
    const auto root = nand / (system_space ? "system" : "user") / "save";
    if (kind == SaveKind::System)
        return root / Hex16(system_save_id) / std::string(32, '0');
    const auto future = kind == SaveKind::Device
                            ? root / "device" / Hex16(title & ~0xFFull) / "0"
                            : root / "account" / raw_uuid / Hex16(title & ~0xFFull) / "0";
    if (std::filesystem::exists(future))
        return future;
    return root / "0000000000000000" / (kind == SaveKind::Device ? std::string(32, '0') : user) /
           Hex16(title);
}
inline std::uint64_t FingerprintSeed(const std::string& name, std::uint64_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : name) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash ^ (size * 0x9E3779B97F4A7C15ull);
}
inline void FingerprintBytes(std::uint64_t& hash, const unsigned char* bytes, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}
inline std::uint64_t AcknowledgedOffset(const std::string& range, std::uint64_t maximum) {
    if (range.empty())
        return 0;
    if (!range.starts_with("bytes=0-"))
        throw std::invalid_argument("invalid upload acknowledgement");
    std::uint64_t last = 0;
    const auto result = std::from_chars(range.data() + 8, range.data() + range.size(), last);
    if (result.ec != std::errc{} || result.ptr != range.data() + range.size() || last >= maximum)
        throw std::invalid_argument("invalid upload acknowledgement");
    return last + 1;
}

constexpr std::uint64_t UploadChunkBytes = 256 * 1024;
struct Chunk {
    std::uint64_t offset, length;
};
inline Chunk NextChunk(std::uint64_t total, std::uint64_t acknowledged) {
    if (acknowledged > total)
        throw std::invalid_argument("upload offset exceeds file");
    return {acknowledged, std::min(UploadChunkBytes, total - acknowledged)};
}
inline std::string ContentRange(Chunk chunk, std::uint64_t total) {
    if (chunk.length == 0)
        return "bytes */" + std::to_string(total);
    return "bytes " + std::to_string(chunk.offset) + "-" +
           std::to_string(chunk.offset + chunk.length - 1) + "/" + std::to_string(total);
}
} // namespace EdenXbox::SaveSync::Policy
