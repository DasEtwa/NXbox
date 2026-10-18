// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/fs/fs_uwp.h"
#include "eden_uwp/firmware.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include "common/fs/fs.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "frontend_common/firmware_files.h"
#include "frontend_common/firmware_install.h"

namespace EdenXbox {
namespace {
std::mutex status_mutex;
FirmwareStatus status;

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
        try {
            for (const auto& drive : drives) {
                for (const wchar_t* name : {L"NXbox\\firmware", L"firmware"}) {
                    const auto item =
                        AwaitBounded(drive.TryGetItemAsync(name), std::chrono::seconds(5));
                    if (item && *item && item->try_as<winrt::Windows::Storage::StorageFolder>()) {
                        roots.emplace_back(std::wstring(drive.Path()));
                        break;
                    }
                }
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("FIRMWARE usb_scan_failed " + winrt::to_string(error.message()));
        }
        for (const auto& folder : FirmwareManager::FirmwareFolders(local_state, roots)) {
            try {
                if (!Common::FS::Exists(folder)) {
                    continue;
                }
                std::vector<std::filesystem::path> files;
                std::vector<std::filesystem::path> subfolders;
                const auto scan = [&](const std::filesystem::path& directory, bool top) {
                    return Common::FS::Uwp::ForEachEntry(
                        directory, [&](const auto& file, const WIN32_FIND_DATAW& data) {
                            const auto attributes = data.dwFileAttributes;
                            if (file.extension() != L".nca") {
                                if (top && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                                    !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                                    subfolders.push_back(file);
                                }
                                return true;
                            }
                            if (attributes &
                                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                                return false;
                            }
                            files.push_back(file);
                            return true;
                        });
                };
                bool scanned = scan(folder, true);
                // TegraExplorer and most guides leave the NCAs one folder deeper
                // ("firmware/Firmware 21.2.0/*.nca"): take the first subfolder that has them.
                std::sort(subfolders.begin(), subfolders.end());
                for (const auto& subfolder : subfolders) {
                    if (!scanned || !files.empty()) {
                        break;
                    }
                    scanned = scan(subfolder, false);
                }
                if (!scanned) {
                    next.install_failed = true;
                    Diagnostic("FIRMWARE scan_failed");
                    break;
                }
                if (files.empty()) {
                    Diagnostic("FIRMWARE no_nca " + Common::FS::PathToUTF8String(folder));
                    continue;
                }
                std::sort(files.begin(), files.end());
                Diagnostic("FIRMWARE install_begin count=" + std::to_string(files.size()));
                const auto result = FirmwareManager::InstallFirmware(
                    system, files, [](size_t, size_t) { return false; });
                next.version = FirmwareManager::InstalledVersion(system);
                if (result != FirmwareManager::InstallResult::Success) {
                    next.install_failed = true;
                    Diagnostic("FIRMWARE install_failed result=" +
                               std::to_string(static_cast<int>(result)) +
                               " count=" + std::to_string(files.size()));
                    break; // Keep the source for retry; do not let another source hide the failure.
                }
                Diagnostic("FIRMWARE install version=" + next.version +
                           " count=" + std::to_string(files.size()));
                const auto destination = FirmwareManager::InstalledFolder(
                    folder, [](const auto& path) { return Common::FS::Exists(path); });
                // StorageFolder's broker can rename a USB folder when ordinary Win32 cannot.
                const auto source_folder = AwaitBounded(
                    winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(folder.native()),
                    std::chrono::seconds(5));
                if (!source_folder) {
                    next.install_failed = true;
                    Diagnostic("FIRMWARE archive_failed source retained for retry");
                } else {
                    source_folder
                        ->RenameAsync(destination.filename().native(),
                                      winrt::Windows::Storage::NameCollisionOption::FailIfExists)
                        .get();
                }
                // LocalState has priority if both local and USB dumps are present.
                break;
            } catch (const winrt::hresult_error& error) {
                next.install_failed = true;
                Diagnostic("FIRMWARE archive_failed " + winrt::to_string(error.message()));
                break;
            } catch (const std::exception& error) {
                next.install_failed = true;
                Diagnostic(std::string("FIRMWARE install_or_archive_failed ") + error.what());
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
