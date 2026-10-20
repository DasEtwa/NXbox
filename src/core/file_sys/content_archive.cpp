// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include <ranges>
#include "common/logging.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/ctr_encryption_layer.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_header_check.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/romfs_read_diagnostics.h"
#include "core/file_sys/romfs_verify.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/loader/loader.h"

#include "core/file_sys/fssystem/fssystem_compression_configuration.h"
#include "core/file_sys/fssystem/fssystem_crypto_configuration.h"
#include "core/file_sys/fssystem/fssystem_nca_file_system_driver.h"

namespace FileSys {

static u8 MasterKeyIdForKeyGeneration(u8 key_generation) {
    return std::max<u8>(key_generation, 1) - 1;
}

NCA::NCA(VirtualFile file_, const NCA* base_nca, bool allow_missing_base, ReadMode mode)
    : file(std::move(file_)), keys{Core::Crypto::KeyManager::Instance()} {
    if (file == nullptr) {
        status = Loader::ResultStatus::ErrorNullFile;
        return;
    }

    reader = std::make_shared<NcaReader>();
    if (Result rc = reader->Initialize(file, GetCryptoConfiguration(), GetNcaCompressionConfiguration()); R_FAILED(rc)) {
        status = Loader::ResultStatus::ErrorBadNCAHeader;
        return;
    }
    has_valid_header = true;

    // Ensure we have the proper key area keys to continue.
    const u8 master_key_id = MasterKeyIdForKeyGeneration(reader->GetKeyGeneration());
    if (!keys.HasKey(Core::Crypto::S128KeyType::KeyArea, master_key_id, reader->GetKeyIndex())) {
        status = Loader::ResultStatus::ErrorMissingKeyAreaKey;
        return;
    }

    RightsId rights_id{};
    reader->GetRightsId(rights_id.data(), rights_id.size());
    if (rights_id != RightsId{}) {
        // External decryption key required; provide it here.
        u128 rights_id_u128;
        std::memcpy(rights_id_u128.data(), rights_id.data(), sizeof(rights_id));

        auto titlekey =
            keys.GetKey(Core::Crypto::S128KeyType::Titlekey, rights_id_u128[1], rights_id_u128[0]);
        if (titlekey == Core::Crypto::Key128{}) {
            status = Loader::ResultStatus::ErrorMissingTitlekey;
            return;
        }

        if (!keys.HasKey(Core::Crypto::S128KeyType::Titlekek, master_key_id)) {
            status = Loader::ResultStatus::ErrorMissingTitlekek;
            return;
        }

        auto titlekek = keys.GetKey(Core::Crypto::S128KeyType::Titlekek, master_key_id);
        Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(titlekek, Core::Crypto::Mode::ECB);
        cipher.Transcode(titlekey.data(), titlekey.size(), titlekey.data(),
                         Core::Crypto::Op::Decrypt);

        reader->SetExternalDecryptionKey(titlekey.data(), titlekey.size());
    }

    if (mode == ReadMode::HeaderOnly) {
        status = Loader::ResultStatus::Success;
        return;
    }

    if (base_nca && !base_nca->reader) {
        LOG_ERROR(Loader, "NXBOX NCA_PAIR FAIL update={} reason=uninitialized_base",
                  file->GetName());
        status = Loader::ResultStatus::ErrorBadNCAHeader;
        return;
    }

    // A patch must use its original content record. Program patches cannot use
    // Control, another program index, or an already patched NCA.
    if (base_nca &&
        (base_nca->is_update || reader->GetContentType() != base_nca->reader->GetContentType() ||
         (reader->GetProgramId() & ~u64{0x800}) !=
             (base_nca->reader->GetProgramId() & ~u64{0x800}) ||
         reader->GetContentIndex() != base_nca->reader->GetContentIndex())) {
        LOG_ERROR(Loader,
                  "NXBOX NCA_PAIR FAIL update={} base={} update_title={:016X} base_title={:016X} "
                  "update_index={} base_index={}",
                  file->GetName(), base_nca->GetName(), reader->GetProgramId(),
                  base_nca->reader->GetProgramId(), reader->GetContentIndex(),
                  base_nca->reader->GetContentIndex());
        status = Loader::ResultStatus::ErrorBadNCAHeader;
        return;
    }

    const s32 fs_count = reader->GetFsCount();
    NcaFileSystemDriver fs(base_nca ? base_nca->reader : nullptr, reader);
    std::vector<VirtualFile> filesystems(fs_count);
    for (s32 i = 0; i < fs_count; i++) {
        NcaFsHeaderReader header_reader;
        NcaFileSystemDriver::StorageContext context{};
        if (Result rc = fs.OpenStorageWithContext(&filesystems[i], &header_reader, i, &context);
            R_FAILED(rc)) {
            if (IsRomfsVerificationEnabled()) {
                LOG_ERROR(Loader,
                          "NXBOX VERIFY_ROMFS MOUNT_ERROR title={:016X} section={} result={:#x}",
                          reader->GetProgramId(), i, rc.raw);
            }
            status = Loader::ResultStatus::ErrorBadNCAHeader;
            return;
        }

        if (header_reader.GetFsType() == NcaFsHeader::FsType::RomFs) {
            files.push_back(filesystems[i]);
            romfs = files.back();
            {
                // The frontend may set the full-scan switch after content discovery.
                // Always retain context; verification policy belongs to the final selection.
                NcaFsHeader header{};
                header_reader.GetRawData(&header, sizeof(header));
                romfs_verification = std::make_unique<RomfsVerification>(
                    romfs, header, std::move(context), reader->GetFsHeaderHash(i),
                    reader->GetProgramId(),
                    fmt::format("nca={} base={} section={}", file->GetName(),
                                base_nca ? base_nca->file->GetName() : "none", i),
                    base_nca != nullptr);
            }
        }

        if (header_reader.GetFsType() == NcaFsHeader::FsType::PartitionFs) {
            auto npfs = std::make_shared<PartitionFilesystem>(filesystems[i]);
            if (npfs->GetStatus() != Loader::ResultStatus::Success) {
                // The usual cause is data that decrypts to garbage: a damaged or incomplete file.
                LOG_WARNING(Loader, "NCA {}: section {} is not a readable partition ({})",
                            file->GetName(), i, Loader::GetResultStatusString(npfs->GetStatus()));
            }
            if (npfs->GetStatus() == Loader::ResultStatus::Success) {
                dirs.push_back(npfs);
                if (IsDirectoryExeFS(npfs)) {
                    exefs = dirs.back();
                } else if (IsDirectoryLogoPartition(npfs)) {
                    logo = dirs.back();
                } else {
                    continue;
                }
            }
        }

        if (header_reader.GetEncryptionType() == NcaFsHeader::EncryptionType::AesCtrEx) {
            is_update = true;
        }
    }

    if (is_update && base_nca == nullptr && !allow_missing_base) {
        status = Loader::ResultStatus::ErrorMissingBKTRBaseRomFS;
    } else {
        status = Loader::ResultStatus::Success;
    }
}

NCA::~NCA() = default;

Loader::ResultStatus NCA::GetStatus() const {
    return status;
}

bool NCA::HasZeroSectionHeader() const {
    if (!has_valid_header)
        return false;
    if (reader->GetFsCount() == 0)
        return true; // A program without any filesystem cannot boot.
    const u64 section_offset = reader->GetFsOffset(0);
    if (FileSys::ZeroNcaSectionHeader(file->ReadBytes(16, section_offset)))
        return true;
    NcaFsHeaderReader header;
    if (R_FAILED(header.Initialize(*reader, 0)))
        return false;
    // Ordinary ExeFS data follows the section's hash layer. Probe its on-disk header too:
    // the hash layer can be downloaded while the partition itself is a preallocated hole.
    // Sparse/compressed/patched layouts need mapping tables, so leave them to the loader.
    if (header.GetFsType() != NcaFsHeader::FsType::PartitionFs || header.ExistsSparseLayer() ||
        header.ExistsCompressionLayer() || header.GetPatchInfo().HasIndirectTable() ||
        header.GetHashType() != NcaFsHeader::HashType::HierarchicalSha256Hash)
        return false;
    const auto& hash = header.GetHashData().hierarchical_sha256_data;
    if (hash.hash_layer_count <= 0 ||
        static_cast<std::size_t>(hash.hash_layer_count) > hash.hash_layer_region.size())
        return false;
    const s64 target = hash.hash_layer_region[hash.hash_layer_count - 1].offset;
    const auto offset = FileSys::NcaSectionProbeOffset(section_offset, reader->GetFsSize(0), target,
                                                       file->GetSize());
    return offset && FileSys::ZeroNcaSectionHeader(file->ReadBytes(16, *offset));
}

bool NCA::HasValidHeader() const {
    return has_valid_header;
}

std::vector<VirtualFile> NCA::GetFiles() const {
    if (status != Loader::ResultStatus::Success) {
        return {};
    }
    return files;
}

std::vector<VirtualDir> NCA::GetSubdirectories() const {
    if (status != Loader::ResultStatus::Success) {
        return {};
    }
    return dirs;
}

std::string NCA::GetName() const {
    return file->GetName();
}

VirtualDir NCA::GetParentDirectory() const {
    return file->GetContainingDirectory();
}

NCAContentType NCA::GetType() const {
    return static_cast<NCAContentType>(reader->GetContentType());
}

u64 NCA::GetTitleId() const {
    if (is_update) {
        return reader->GetProgramId() | 0x800;
    }
    return reader->GetProgramId();
}

RightsId NCA::GetRightsId() const {
    RightsId result;
    reader->GetRightsId(result.data(), result.size());
    return result;
}

u32 NCA::GetSDKVersion() const {
    return reader->GetSdkAddonVersion();
}

u8 NCA::GetKeyGeneration() const {
    return reader->GetKeyGeneration();
}

bool NCA::IsUpdate() const {
    return is_update;
}

VirtualFile NCA::GetRomFS() const {
    return romfs;
}

void NCA::VerifyRomFS(VirtualFile received, bool full) const {
    if (romfs_verification) {
        romfs_verification->Verify(std::move(received), full);
    } else {
        LOG_ERROR(Loader,
                  "NXBOX ROMFS_CHECK FAIL blocks=0 bad=1 first_bad_offset=none "
                  "source=base title={:016X} reason=no_mount_context",
                  GetTitleId());
    }
}

VirtualDir NCA::GetExeFS() const {
    return exefs;
}

VirtualFile NCA::GetBaseFile() const {
    return file;
}

VirtualDir NCA::GetLogoPartition() const {
    return logo;
}

} // namespace FileSys
