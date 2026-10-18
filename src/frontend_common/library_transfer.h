// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace LibraryTransfer {
namespace fs = std::filesystem;
enum class Result { Success, NoDrive, NotInternal, NoSpace, Conflict, Failed };
struct File {
    fs::path source;
    fs::path target;
    std::uint64_t size;
};
struct IO {
    std::function<bool(const fs::path&)> exists;
    std::function<std::uint64_t(const fs::path&)> size;
    std::function<bool(const File&)> unchanged;
    std::function<bool(const File&)> copy;
    std::function<bool(const fs::path&)> remove;
};
// Refuse before writing if any destination collides or capacity is unknown. Originals survive
// every copy/verification failure; delete only after the entire group has been verified.
inline Result Move(const std::vector<File>& files, std::optional<std::uint64_t> available,
                   const IO& io) {
    if (files.empty())
        return Result::NotInternal;
    std::uint64_t required = 64ull << 20;
    for (const auto& file : files) {
        if (file.size == 0 || file.size > (std::numeric_limits<std::uint64_t>::max)() - required)
            return Result::Failed;
        required += file.size;
        if (io.exists(file.target))
            return Result::Conflict;
        if (!io.unchanged(file))
            return Result::Failed;
    }
    if (!available || *available < required)
        return Result::NoSpace;
    for (const auto& file : files)
        if (!io.copy(file) || io.size(file.target) != file.size)
            return Result::Failed;
    for (const auto& file : files)
        if (!io.unchanged(file) || io.size(file.target) != file.size)
            return Result::Failed;
    for (const auto& file : files)
        if (!io.unchanged(file) || !io.remove(file.source))
            return Result::Failed;
    return Result::Success;
}
} // namespace LibraryTransfer
