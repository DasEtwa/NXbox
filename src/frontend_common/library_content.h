// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
#include "eden_uwp/ui/strings.h"

namespace LibraryContent {
enum class Kind { Unknown, Application, Patch, AddOnContent };
enum class Problem { None, UpdateOnly, DlcOnly, Corrupted, MissingKeys, MissingFirmware };
struct Meta {
    std::uint64_t title_id;
    Kind kind;
};
struct Classification {
    Kind kind = Kind::Unknown;
    std::vector<std::uint64_t> bases;
};
// Application metadata wins over bundled updates/DLC; filenames never determine content type.
inline Classification Classify(std::span<const Meta> entries) {
    Classification result;
    for (const auto& entry : entries) {
        if (entry.kind == Kind::Application)
            return {Kind::Application, {}};
        if (entry.kind == Kind::Patch || entry.kind == Kind::AddOnContent) {
            if (result.kind == Kind::Unknown || entry.kind == Kind::Patch)
                result.kind = entry.kind;
            const auto base = entry.kind == Kind::Patch ? entry.title_id & ~std::uint64_t{0x800}
                                                        : entry.title_id & ~std::uint64_t{0x1fff};
            result.bases.push_back(base);
        }
    }
    return result;
}
inline Problem Standalone(Kind kind) {
    return kind == Kind::Patch          ? Problem::UpdateOnly
           : kind == Kind::AddOnContent ? Problem::DlcOnly
                                        : Problem::None;
}
inline bool Attached(const Classification& content, std::span<const std::uint64_t> bases) {
    return !content.bases.empty() &&
           std::all_of(content.bases.begin(), content.bases.end(), [&](auto base) {
               return std::find(bases.begin(), bases.end(), base) != bases.end();
           });
}
template <typename Entry, typename CompareName>
bool Less(const Entry& left, const Entry& right, CompareName compare_name) {
    if (left.incomplete != right.incomplete)
        return right.incomplete;
    const int order = compare_name(left.name, right.name);
    return order != 0 ? order < 0 : left.path < right.path;
}

// The caller subtracts SystemResultStatus::ErrorLoader before passing a loader status.
template <typename Status>
Problem LoadProblem(Status status) {
    switch (status) {
    case Status::ErrorMissingProductionKeyFile:
    case Status::ErrorMissingHeaderKey:
    case Status::ErrorIncorrectHeaderKey:
    case Status::ErrorMissingTitlekey:
    case Status::ErrorMissingTitlekek:
    case Status::ErrorMissingKeyAreaKey:
    case Status::ErrorIncorrectKeyAreaKey:
    case Status::ErrorIncorrectTitlekeyOrTitlekek:
        return Problem::MissingKeys;
    case Status::ErrorMissingBKTRBaseRomFS:
        return Problem::UpdateOnly;
    case Status::ErrorBadPFSHeader:
    case Status::ErrorIncorrectPFSFileSize:
    case Status::ErrorBadNCAHeader:
    case Status::ErrorNoExeFS:
    case Status::ErrorNoRomFS:
    case Status::ErrorMissingNPDM:
    case Status::ErrorIntegrityVerificationFailed:
        return Problem::Corrupted;
    default:
        return Problem::None;
    }
}
inline EdenXbox::Ui::Text Label(Problem problem) {
    using EdenXbox::Ui::Text;
    switch (problem) {
    case Problem::UpdateOnly:
        return Text::UpdateWithoutBase;
    case Problem::DlcOnly:
        return Text::DlcWithoutBase;
    case Problem::Corrupted:
        return Text::IncompleteFile;
    case Problem::MissingKeys:
        return Text::LaunchMissingKeys;
    case Problem::MissingFirmware:
        return Text::FirmwareMissing;
    default:
        return Text::LaunchFailed;
    }
}
} // namespace LibraryContent
