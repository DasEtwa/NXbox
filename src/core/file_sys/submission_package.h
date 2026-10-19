// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs.h"

namespace Core::Crypto {
class KeyManager;
}

namespace Loader {
enum class ResultStatus : u16;
}

namespace FileSys {

class NCA;
class PartitionFilesystem;

enum class ContentRecordType : u8;

class NSP : public ReadOnlyVfsDirectory {
public:
    explicit NSP(VirtualFile file_, u64 title_id = 0, std::size_t program_index_ = 0,
                 bool program_headers_only = false);
    ~NSP() override;

    Loader::ResultStatus GetStatus() const;
    Loader::ResultStatus GetProgramStatus() const;
    // Should only be used when one title id can be assured.
    u64 GetProgramTitleID() const;
    u64 GetExtractedTitleID() const;
    std::vector<u64> GetProgramTitleIDs() const;

    // Parsed CNMT identities, including titles whose program NCA failed to open.
    const std::vector<std::pair<u64, TitleType>>& GetContentMeta() const;
    bool HasCorruptProgram() const;
    bool HasMissingProgramKeys() const;
    bool IsExtractedType() const;

    // Common (Can be safely called on both types)
    VirtualFile GetRomFS() const;
    VirtualDir GetExeFS() const;

    // Type 0 Only (Collection of NCAs + Certificate + Ticket + Meta XML)
    std::vector<std::shared_ptr<NCA>> GetNCAsCollapsed() const;
    std::multimap<u64, std::shared_ptr<NCA>> GetNCAsByTitleID() const;
    std::map<u64, std::map<std::pair<TitleType, ContentRecordType>, std::shared_ptr<NCA>>> GetNCAs()
        const;
    std::shared_ptr<NCA> GetNCA(u64 title_id, ContentRecordType type,
                                TitleType title_type = TitleType::Application) const;
    VirtualFile GetNCAFile(u64 title_id, ContentRecordType type,
                           TitleType title_type = TitleType::Application) const;

    std::vector<VirtualFile> GetFiles() const override;

    std::vector<VirtualDir> GetSubdirectories() const override;

    std::string GetName() const override;

    VirtualDir GetParentDirectory() const override;

private:
    void SetTicketKeys(const std::vector<VirtualFile>& files);
    void InitializeExeFSAndRomFS(const std::vector<VirtualFile>& files);
    void ReadNCAs(const std::vector<VirtualFile>& files);

    VirtualFile file;

    const u64 expected_program_id;
    const std::size_t program_index;
    const bool program_headers_only;

    std::vector<std::pair<u64, TitleType>> content_meta;
    bool corrupt_program = false;
    bool missing_program_keys = false;
    bool extracted = false;
    Loader::ResultStatus status;
    std::map<u64, Loader::ResultStatus> program_status;

    std::shared_ptr<PartitionFilesystem> pfs;
    // Map title id -> {map type -> NCA}
    std::map<u64, std::map<std::pair<TitleType, ContentRecordType>, std::shared_ptr<NCA>>> ncas;
    std::set<u64> program_ids;
    std::vector<VirtualFile> ticket_files;

    Core::Crypto::KeyManager& keys;

    VirtualFile romfs;
    VirtualDir exefs;
};
} // namespace FileSys
