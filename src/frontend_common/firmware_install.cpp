// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend_common/firmware_install.h"

#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/romfs.h"
#include "core/hle/service/set/settings_types.h"
#include "frontend_common/content_manager.h"
#include "frontend_common/firmware_files.h"
#include "frontend_common/firmware_sources.h"

namespace FirmwareManager {
namespace {
std::string ArchiveVersion(const FileSys::NCA& nca) {
    if (nca.GetStatus() != Loader::ResultStatus::Success || !nca.GetRomFS()) {
        return {};
    }
    const auto romfs = FileSys::ExtractRomFS(nca.GetRomFS());
    const auto file = romfs ? romfs->GetFile("file") : nullptr;
    if (!file || file->GetSize() != sizeof(Service::Set::FirmwareVersionFormat)) {
        return {};
    }
    Service::Set::FirmwareVersionFormat version{};
    if (file->Read(reinterpret_cast<u8*>(&version), sizeof(version), 0) != sizeof(version)) {
        return {};
    }
    return DisplayVersion({version.display_version.data(), version.display_version.size()});
}
} // namespace

std::string InstalledVersion(Core::System& system) {
    const auto cache = system.GetFileSystemController().GetSystemNANDContents();
    const auto nca =
        cache ? cache->GetEntry(0x0100000000000809ULL, FileSys::ContentRecordType::Data) : nullptr;
    return nca ? ArchiveVersion(*nca) : std::string{};
}

std::string VerifyFirmware(Core::System& system, const std::vector<std::filesystem::path>& files,
                           const std::function<bool(size_t, size_t)>& callback) {
    if (files.empty()) {
        return {};
    }
    auto& vfs = *system.GetFilesystem();
    // Decrypt headers with the user's loaded keys and verify each source before deleting anything.
    std::string version;
    bool has_mii_model = false;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto file =
            vfs.OpenFile(Common::FS::PathToUTF8String(files[i]), FileSys::OpenMode::Read);
        const auto name = Common::FS::PathToUTF8String(files[i].filename());
        if (!file || !file->GetSize()) {
            LOG_ERROR(Frontend, "Firmware NCA {} is missing or empty", name);
            return {};
        }
        const FileSys::NCA nca(file);
        if (nca.GetStatus() != Loader::ResultStatus::Success) {
            LOG_ERROR(Frontend, "Firmware NCA {} failed to parse: {}", name,
                      Loader::GetResultStatusString(nca.GetStatus()));
            return {};
        }
        if (nca.GetTitleId() == 0x0100000000000809ULL &&
            nca.GetType() == FileSys::NCAContentType::Data) {
            version = ArchiveVersion(nca);
        }
        // Retail dumps ship the Mii model archive as PublicData, not Data.
        if (nca.GetTitleId() == 0x0100000000000802ULL &&
            (nca.GetType() == FileSys::NCAContentType::Data ||
             nca.GetType() == FileSys::NCAContentType::PublicData) &&
            nca.GetRomFS()) {
            has_mii_model = true;
        }
        Loader::AppLoader_NCA loader(file);
        bool cancelled = false;
        const auto status = loader.VerifyIntegrity([&](size_t processed, size_t total) {
            cancelled = callback(total, processed);
            return !cancelled;
        });
        if (cancelled || status != Loader::ResultStatus::Success) {
            LOG_ERROR(Frontend, "Firmware NCA {} failed integrity check: {}", name,
                      Loader::GetResultStatusString(status));
            return {};
        }
    }
    if (version.empty() || !has_mii_model) {
        LOG_ERROR(Frontend, "Firmware set incomplete: system_version={} mii_model={}",
                  !version.empty(), has_mii_model);
        return {};
    }
    return version;
}

InstallResult InstallFirmware(Core::System& system, const std::vector<std::filesystem::path>& files,
                              const std::function<bool(size_t, size_t)>& callback) {
    if (files.empty())
        return InstallResult::NoNCAs;
    const auto version = VerifyFirmware(system, files, callback);
    if (version.empty())
        return InstallResult::FailedCorrupted;
    if (Sources::OlderVersion(version, InstalledVersion(system))) {
        LOG_WARNING(Frontend, "source_older_than_installed");
        return InstallResult::FailedCorrupted;
    }
    auto& vfs = *system.GetFilesystem();
    auto& controller = system.GetFileSystemController();
    const auto content = controller.GetSystemNANDContentDirectory();
    if (!content || !content->IsWritable() || !content->CleanSubdirectoryRecursive("registered")) {
        return InstallResult::FailedDelete;
    }
    const auto registered = content->GetDirectoryRelative("registered");
    if (!registered) {
        return InstallResult::FailedCopy;
    }
    // Keep the desktop layout and raw copy semantics, including metadata NCAs.
    for (size_t i = 0; i < files.size(); ++i) {
        const auto source =
            vfs.OpenFile(Common::FS::PathToUTF8String(files[i]), FileSys::OpenMode::Read);
        const auto destination =
            registered->CreateFileRelative(Common::FS::PathToUTF8String(files[i].filename()));
        if (!FileSys::VfsRawCopy(source, destination) || callback(files.size(), i + 1)) {
            controller.CreateFactories(vfs);
            return InstallResult::FailedCopy;
        }
    }
    controller.CreateFactories(vfs);
    FileSys::ManualContentProvider provider;
    bool cancelled = false;
    const auto verify_callback = [&](size_t total, size_t processed) {
        cancelled = callback(total, processed);
        return cancelled;
    };
    const auto failures =
        ContentManager::VerifyInstalledContents(system, provider, verify_callback, true);
    for (const auto& failure : failures) {
        LOG_ERROR(Frontend, "Installed firmware content failed verification: {}", failure);
    }
    if (cancelled || !failures.empty() || InstalledVersion(system).empty()) {
        return InstallResult::FailedCorrupted;
    }
    return InstallResult::Success;
}
} // namespace FirmwareManager
