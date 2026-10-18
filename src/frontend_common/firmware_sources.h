// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace FirmwareManager::Sources {
namespace fs = std::filesystem;
using Bytes = std::span<std::uint8_t>;
using View = std::span<const std::uint8_t>;
struct Reader {
    std::uint64_t size{};
    std::function<void(std::uint64_t, Bytes)> read;
    void Read(std::uint64_t offset, Bytes bytes) const;
};
struct Writer {
    virtual ~Writer() = default;
    virtual void Write(View bytes) = 0;
    virtual void Finish() = 0;
};
struct Entry {
    fs::path path;
    bool directory{}, reparse{};
    std::uint64_t size{}, modified{};
};
// All host access is supplied by the caller; UWP uses broker-aware operations.
struct IO {
    std::function<std::vector<Entry>(const fs::path&)> list;
    std::function<Reader(const fs::path&)> open;
    std::function<std::unique_ptr<Writer>(const fs::path&)> create;
    std::function<void(const fs::path&)> mkdir;
    std::function<void(const fs::path&)> remove_tree;
    std::function<void(const std::string&)> log;
};
enum class Kind { Folder, Zip, Nand };
struct Source {
    Kind kind;
    fs::path path;
    std::vector<Entry> parts;
    // Legacy archive location metadata; the UWP scanner records fingerprints without renaming.
    fs::path archive_folder;
    bool recursive{true};
};
// Firmware folders recurse at most eight levels, root/NXbox discovery is shallow except emuMMC.
std::vector<Source> Discover(const IO& io, const std::vector<fs::path>& firmware_folders,
                             const std::vector<fs::path>& drive_roots);
bool OlderVersion(const std::string& source, const std::string& installed);
std::string ScanRecords(const std::vector<Source>& sources);
void SweepTemporary(const IO& io, const fs::path& local_state);
std::string Fingerprint(const Source& source);
Reader Joined(const IO& io, const std::vector<Entry>& parts);
// Sector index is relative to SYSTEM, not to the GPP image. Caller supplies AES-128-XTS with
// Nintendo's big-endian tweak and a 0x4000-byte sector (Eden AESCipher::XTSTranscode).
using Decrypt = std::function<void(std::uint64_t, View, Bytes)>;
// Optional candidates receives one independent NCA set per direct parent directory.
std::vector<fs::path> Extract(const IO& io, const Source& source, const fs::path& temporary,
                              const Decrypt& decrypt,
                              std::vector<std::vector<fs::path>>* candidates = nullptr);
} // namespace FirmwareManager::Sources
