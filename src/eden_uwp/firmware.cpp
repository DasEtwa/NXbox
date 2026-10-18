// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/fs/fs_uwp.h"
#include "eden_uwp/firmware.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "core/core.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "frontend_common/firmware_files.h"
#include "frontend_common/firmware_install.h"
#include "frontend_common/firmware_sources.h"

namespace EdenXbox {
namespace {
std::mutex status_mutex;
FirmwareStatus status;

namespace Sources = FirmwareManager::Sources;

Sources::IO FirmwareIO() {
    Sources::IO io;
    io.list = [](const std::filesystem::path& path) {
        std::vector<Sources::Entry> entries;
        if (!Common::FS::Exists(path))
            return entries;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Firmware directory is inaccessible or a reparse point");
        if (!Common::FS::Uwp::ForEachEntry(path, [&](const auto& child,
                                                     const WIN32_FIND_DATAW& data) {
                entries.push_back({child, bool(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                                   bool(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                                   (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                                   (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                       data.ftLastWriteTime.dwLowDateTime});
                return true;
            })) {
            throw std::runtime_error("Firmware directory enumeration failed");
        }
        return entries;
    };
    io.open = [](const std::filesystem::path& path) {
        auto file = std::make_shared<Common::FS::IOFile>(path, Common::FS::FileAccessMode::Read);
        if (!file->IsOpen())
            throw std::runtime_error("Firmware source open failed");
        return Sources::Reader{file->GetSize(), [file](std::uint64_t offset, Sources::Bytes bytes) {
                                   if (!file->Seek(static_cast<s64>(offset)) ||
                                       file->ReadSpan(bytes) != bytes.size())
                                       throw std::runtime_error("Firmware source read failed");
                               }};
    };
    struct FileWriter final : Sources::Writer {
        Common::FS::IOFile file;
        explicit FileWriter(const std::filesystem::path& path)
            : file(path, Common::FS::FileAccessMode::Write) {
            if (!file.IsOpen())
                throw std::runtime_error("Firmware temporary open failed");
        }
        void Write(Sources::View bytes) override {
            if (file.WriteSpan(bytes) != bytes.size())
                throw std::runtime_error("Firmware temporary write failed (drive full?)");
        }
        void Finish() override {
            if (!file.Flush())
                throw std::runtime_error("Firmware temporary flush failed");
            file.Close();
        }
    };
    io.create = [](const auto& path) { return std::make_unique<FileWriter>(path); };
    io.mkdir = [](const auto& path) {
        if (Common::FS::Exists(path) || !Common::FS::CreateDir(path))
            throw std::runtime_error("Firmware temporary directory creation failed");
    };
    io.remove_tree = [](const auto& path) {
        if (!Common::FS::RemoveDirRecursively(path))
            Diagnostic("FIRMWARE temporary_cleanup_failed");
    };
    io.log = [](const auto& text) { Diagnostic(text); };
    return io;
}

// The library's USB import copies the keys only after this runs, so the first launch with a fresh
// drive would try its firmware without keys and fail. Take missing keys from the drive first, from
// the same places the import looks (switch\ first, then the drive root).
void CopyMissingKeys(const std::filesystem::path& local_state,
                     const std::vector<winrt::Windows::Storage::StorageFolder>& drives) {
    using namespace winrt::Windows::Storage;
    for (const wchar_t* name : {L"prod.keys", L"title.keys"}) {
        if (Common::FS::Exists(local_state / "eden" / "keys" / name)) {
            continue;
        }
        bool copied = false;
        for (const auto& drive : drives) {
            for (const wchar_t* prefix : {L"switch\\", L""}) {
                if (copied) {
                    break;
                }
                try {
                    const auto item =
                        AwaitBounded(drive.TryGetItemAsync(std::wstring(prefix) + name),
                                     std::chrono::seconds(3));
                    const auto file = item && *item ? item->try_as<StorageFile>() : nullptr;
                    if (!file) {
                        continue;
                    }
                    const auto keys =
                        AwaitBounded(ApplicationData::Current().LocalFolder().CreateFolderAsync(
                                         L"eden\\keys", CreationCollisionOption::OpenIfExists),
                                     std::chrono::seconds(5));
                    copied = keys && AwaitBounded(file.CopyAsync(*keys, name,
                                                                 NameCollisionOption::FailIfExists),
                                                  std::chrono::seconds(10));
                    Diagnostic("FIRMWARE keys_from_usb " + winrt::to_string(name) +
                               (copied ? " copied" : " copy_failed"));
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("FIRMWARE keys_from_usb_failed " +
                               winrt::to_string(error.message()));
                }
            }
        }
    }
}
} // namespace

FirmwareStatus GetFirmwareStatus() {
    const std::scoped_lock lock(status_mutex);
    return status;
}

void PrepareFirmware(const std::filesystem::path& local_state) {
    FirmwareStatus next;
    try {
        std::vector<winrt::Windows::Storage::StorageFolder> drives;
        try {
            const auto found = AwaitBounded(
                winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync(),
                std::chrono::seconds(5));
            if (found) {
                drives.assign(begin(*found), end(*found));
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("FIRMWARE usb_scan_failed " + winrt::to_string(error.message()));
        }
        CopyMissingKeys(local_state, drives);
        // Filesystem-only setup avoids reserving guest memory just to inspect/install firmware.
        Core::Crypto::KeyManager::Instance().ReloadKeys();
        Core::System system;
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        auto& controller = system.GetFileSystemController();
        controller.CreateFactories(*system.GetFilesystem());
        next.version = FirmwareManager::InstalledVersion(system);
        std::vector<std::filesystem::path> roots;
        for (const auto& drive : drives)
            roots.emplace_back(std::wstring(drive.Path()));
        const auto io = FirmwareIO();
        const auto sources =
            Sources::Discover(io, FirmwareManager::FirmwareFolders(local_state, roots), roots);
        const auto record_path = local_state / "firmware_source.txt";
        auto records = Common::FS::ReadStringFromFile(record_path, Common::FS::FileType::TextFile);
        for (const auto& source : sources) {
            // Each attempt owns a fresh directory beside its source, including for USB NANDs.
            const auto temporary = source.path.parent_path() /
                                   (".firmware_extract-" + std::to_string(GetCurrentProcessId()) +
                                    "-" + std::to_string(GetTickCount64()));
            bool temporary_created = false;
            auto attempt_io = io;
            attempt_io.mkdir = [&](const auto& path) {
                io.mkdir(path);
                temporary_created = true;
            };
            struct Cleanup {
                const Sources::IO& io;
                std::filesystem::path path;
                const bool& created;
                ~Cleanup() {
                    if (created && Common::FS::Exists(path))
                        io.remove_tree(path);
                }
            } cleanup{io, temporary, temporary_created};
            bool source_installed = false;
            try {
                const auto fingerprint = "BEGIN\n" + Sources::Fingerprint(source) + "END\n";
                if (!next.version.empty() && source.archive_folder.empty() &&
                    !source.parts.empty() && records.find(fingerprint) != std::string::npos) {
                    Diagnostic("FIRMWARE source_already_installed");
                    continue;
                }
                Sources::Decrypt decrypt;
                const auto key = Core::Crypto::KeyManager::Instance().GetBISKey(2);
                if (std::any_of(key.begin(), key.end(), [](auto byte) { return byte != 0; })) {
                    auto cipher = std::make_shared<Core::Crypto::AESCipher<Core::Crypto::Key256>>(
                        key, Core::Crypto::Mode::XTS);
                    decrypt = [cipher](std::uint64_t sector, Sources::View input,
                                       Sources::Bytes output) {
                        cipher->XTSTranscode(input.data(), input.size(), output.data(),
                                             static_cast<size_t>(sector), 0x4000,
                                             Core::Crypto::Op::Decrypt);
                    };
                } else if (source.kind == Sources::Kind::Nand) {
                    throw std::runtime_error("NAND requires bis_key_02 in prod.keys");
                }
                const auto files = Sources::Extract(attempt_io, source, temporary, decrypt);
                if (files.empty()) {
                    Diagnostic("FIRMWARE no_nca " + Common::FS::PathToUTF8String(source.path));
                    continue;
                }
                Diagnostic("FIRMWARE install_begin count=" + std::to_string(files.size()));
                const auto result = FirmwareManager::InstallFirmware(
                    system, files, [](size_t, size_t) { return false; });
                next.version = FirmwareManager::InstalledVersion(system);
                if (result != FirmwareManager::InstallResult::Success) {
                    next.install_failed = true;
                    Diagnostic("FIRMWARE install_failed result=" +
                               std::to_string(static_cast<int>(result)));
                    continue;
                }
                source_installed = true;
                next.install_failed = false;
                Diagnostic("FIRMWARE install version=" + next.version +
                           " count=" + std::to_string(files.size()));
                // Remove temporary chunks before renaming the enclosing source folder.
                io.remove_tree(temporary);
                if (!source.archive_folder.empty()) {
                    const auto destination = FirmwareManager::InstalledFolder(
                        source.archive_folder,
                        [](const auto& path) { return Common::FS::Exists(path); });
                    const auto folder =
                        AwaitBounded(winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                                         source.archive_folder.native()),
                                     std::chrono::seconds(5));
                    if (!folder) {
                        next.install_failed = true;
                        Diagnostic("FIRMWARE archive_failed source retained for retry");
                    } else {
                        folder
                            ->RenameAsync(
                                destination.filename().native(),
                                winrt::Windows::Storage::NameCollisionOption::FailIfExists)
                            .get();
                    }
                } else if (!source.parts.empty()) {
                    records += fingerprint;
                    if (Common::FS::WriteStringToFile(record_path, Common::FS::FileType::TextFile,
                                                      records) != records.size()) {
                        next.install_failed = true;
                        Diagnostic("FIRMWARE source_record_failed source retained for retry");
                    }
                }
                break; // First successful source wins; LocalState is first.
            } catch (const winrt::hresult_error& error) {
                next.install_failed = true;
                Diagnostic("FIRMWARE install_or_archive_failed " +
                           winrt::to_string(error.message()));
                if (source_installed)
                    break;
            } catch (const std::exception& error) {
                next.install_failed = true;
                Diagnostic(std::string("FIRMWARE install_or_archive_failed ") + error.what());
                if (source_installed)
                    break;
            }
        }
        next.home_available = controller.GetSystemNANDContents()->GetEntryRaw(
                                  static_cast<u64>(Service::AM::AppletProgramId::QLaunch),
                                  FileSys::ContentRecordType::Program) != nullptr;
        next.mii_edit_available = controller.GetSystemNANDContents()->GetEntryRaw(
                                      static_cast<u64>(Service::AM::AppletProgramId::MiiEdit),
                                      FileSys::ContentRecordType::Program) != nullptr;
        Diagnostic("FIRMWARE installed version=" +
                   (next.version.empty() ? std::string("none") : next.version));
    } catch (const winrt::hresult_error& error) {
        next.install_failed = true;
        Diagnostic("FIRMWARE prepare_failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        next.install_failed = true;
        Diagnostic(std::string("FIRMWARE prepare_failed ") + error.what());
    }
    const std::scoped_lock lock(status_mutex);
    status = std::move(next);
}
} // namespace EdenXbox
