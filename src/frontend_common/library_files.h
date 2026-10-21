// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/file_sys/pfs0_extent.h"

namespace LibraryFiles {
inline std::wstring Fold(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

// Roots come from the library's storage discovery, never from the selected filename.
inline bool Allowed(const std::filesystem::path& file,
                    const std::vector<std::filesystem::path>& roots) {
    if (!file.is_absolute() || file.filename().empty())
        return false;
    for (const auto& part : file) {
        if (part == ".." || part == ".")
            return false;
    }
    if (file.filename().wstring().find(L':') != std::wstring::npos)
        return false; // No alternate data streams.
    for (const auto& root : roots) {
        auto r = root.begin();
        auto p = file.begin();
        for (; r != root.end() && p != file.end() && Fold(r->wstring()) == Fold(p->wstring());
             ++r, ++p) {
        }
        if (r == root.end() && p != file.end())
            return true;
    }
    return false;
}

inline std::filesystem::path PackagePath(std::filesystem::path path) {
    if (Fold(path.extension().wstring()) == L".partial")
        path.replace_extension();
    return path;
}
inline bool IsPackage(const std::filesystem::path& path) {
    const auto ext = Fold(PackagePath(path).extension().wstring());
    return ext == L".nsp" || ext == L".nsz" || ext == L".xci" || ext == L".nro";
}
inline bool Sibling(const std::filesystem::path& base, const std::filesystem::path& candidate) {
    // A homebrew app and an NSP with the same stem are separate library entries.
    const bool base_nro = Fold(PackagePath(base).extension().wstring()) == L".nro";
    const bool candidate_nro = Fold(PackagePath(candidate).extension().wstring()) == L".nro";
    return IsPackage(candidate) && base_nro == candidate_nro &&
           Fold(base.parent_path().wstring()) == Fold(candidate.parent_path().wstring()) &&
           Fold(PackagePath(base).stem().wstring()) ==
               Fold(PackagePath(candidate).stem().wstring());
}

// Cached IDs must be exactly 16 hexadecimal digits; malformed cache data must not
// throw or silently associate unrelated packages through a partially parsed ID.
inline std::optional<std::uint64_t> ParseTitle(std::string_view text) {
    if (text.size() != 16)
        return std::nullopt;
    std::uint64_t id = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id, 16);
    if (error != std::errc{} || end != text.data() + text.size() || id == 0)
        return std::nullopt;
    return id;
}

// Only an explicit bracketed title ID associates filenames; fuzzy game-name matches risk data loss.
inline std::optional<std::uint64_t> FilenameTitle(const std::filesystem::path& path) {
    const auto name = path.filename().wstring();
    for (std::size_t i = 0; i + 18 <= name.size(); ++i) {
        if (name[i] != L'[' || name[i + 17] != L']')
            continue;
        std::uint64_t id = 0;
        bool valid = true;
        for (std::size_t j = i + 1; j < i + 17; ++j) {
            const auto c = std::towlower(name[j]);
            const int digit = c >= L'0' && c <= L'9'   ? c - L'0'
                              : c >= L'a' && c <= L'f' ? c - L'a' + 10
                                                       : -1;
            if (digit < 0) {
                valid = false;
                break;
            }
            id = (id << 4) | digit;
        }
        if (valid && id != 0)
            return id;
    }
    return std::nullopt;
}
inline bool Related(std::uint64_t base, std::uint64_t candidate) {
    return base && candidate && base != candidate &&
           (base & ~std::uint64_t{0x1fff}) == (candidate & ~std::uint64_t{0x1fff});
}

// Bounded, key-free header inspection. Unrecognized full headers never authorize replacement.
// read(offset, size) returns exactly the bytes available, as unsigned bytes.
template <typename Read>
std::optional<bool> TruncatedPfs0(std::uint64_t size, Read read) {
    const auto header = read(0, static_cast<std::size_t>(std::min<std::uint64_t>(size, 16)));
    if (header.size() != std::min<std::uint64_t>(size, 16))
        return std::nullopt; // An I/O failure does not authorize overwriting an existing file.
    if (size < 16)
        return true; // An NSP cannot contain even its mandatory header.
    if (header[0] != 'P' || header[1] != 'F' || header[2] != 'S' || header[3] != '0')
        return std::nullopt;
    const auto le = [](const auto& bytes, std::size_t offset, int count) {
        std::uint64_t value = 0;
        for (int i = 0; i < count; ++i)
            value |= std::uint64_t{bytes[offset + i]} << (8 * i);
        return value;
    };
    const auto count = le(header, 4, 4);
    const auto strings = le(header, 8, 4);
    if (count > 65535 || strings > 16 * 1024 * 1024)
        return std::nullopt;
    const auto data_offset = 16 + count * 24 + strings;
    if (data_offset > size)
        return true;
    const auto table = read(16, static_cast<std::size_t>(count * 24));
    if (table.size() != count * 24)
        return std::nullopt;
    std::vector<FileSys::Pfs0Span> spans;
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = le(table, i * 24, 8);
        if (offset > (std::numeric_limits<std::uint64_t>::max)() - data_offset)
            return true;
        spans.push_back({std::to_string(i), data_offset + offset, le(table, i * 24 + 8, 8)});
    }
    return FileSys::MeasurePfs0(spans, size).Truncated();
}
} // namespace LibraryFiles
