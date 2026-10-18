// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

// NXbox Pack sends a game above 1 GiB as `<name>.nxpart000`, `<name>.nxpart001`, ... followed by a
// `<name>.nxparts` manifest ("parts=<N>\nsize=<bytes>\n"). The manifest goes last, so its presence
// means every part is on the console. JoinSplitUploads() turns them back into `<name>`.
namespace SplitUpload {

struct JoinResult {
    unsigned joined = 0;  // manifests whose game is complete and whose parts are gone
    unsigned skipped = 0; // manifests left for a later start (broken, no space, I/O error)
};

using Log = std::function<void(const std::string&)>;

namespace detail {
namespace fs = std::filesystem;

constexpr std::size_t kBufferSize = 4 * 1024 * 1024;
// 4096 parts of 1 GiB is far beyond any game; a larger count is a corrupt manifest.
constexpr std::uint64_t kMaxParts = 4096;

inline std::string Utf8(const fs::path& path) {
    const auto text = path.u8string(); // std::string before C++20, std::u8string after
    return std::string(text.begin(), text.end());
}

inline fs::path PartPath(const fs::path& target, std::uint64_t index) {
    std::string digits = std::to_string(index);
    digits.insert(0, digits.size() < 3 ? 3 - digits.size() : 0, '0');
    fs::path part = target;
    part += ".nxpart" + digits;
    return part;
}

inline std::uint64_t SizeOrZero(const fs::path& path) {
    std::error_code error;
    const auto size = fs::is_regular_file(path, error) ? fs::file_size(path, error) : 0;
    return error ? 0 : static_cast<std::uint64_t>(size);
}

// Reads "parts=<N>" and "size=<bytes>"; a missing or non-numeric field makes it unusable.
inline bool ReadManifest(const fs::path& manifest, std::uint64_t& parts, std::uint64_t& size) {
    std::ifstream in(manifest);
    bool have_parts = false;
    bool have_size = false;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const char* const begin = line.data() + equals + 1;
        const char* const end = line.data() + line.size();
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            continue;
        }
        if (key == "parts") {
            parts = value;
            have_parts = true;
        } else if (key == "size") {
            size = value;
            have_size = true;
        }
    }
    return have_parts && have_size;
}

inline void Append(const fs::path& target, const fs::path& part) {
    std::ifstream in(part, std::ios::binary);
    std::ofstream out(target, std::ios::binary | std::ios::app);
    if (!in || !out) {
        throw std::runtime_error("cannot open " + Utf8(part.filename()));
    }
    std::vector<char> buffer(kBufferSize);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0 && !out.write(buffer.data(), got)) {
            throw std::runtime_error("cannot write " + Utf8(target.filename()));
        }
    }
    if (in.bad()) {
        throw std::runtime_error("cannot read " + Utf8(part.filename()));
    }
    out.flush();
    out.close();
    if (out.fail()) {
        throw std::runtime_error("cannot write " + Utf8(target.filename()));
    }
}

// Returns true when the game is complete and the manifest is gone.
inline bool JoinOne(const fs::path& manifest, const Log& log) {
    fs::path target = manifest;
    target.replace_extension(); // drops ".nxparts"
    const std::string name = Utf8(target.filename());
    std::uint64_t parts = 0;
    std::uint64_t size = 0;
    if (!ReadManifest(manifest, parts, size) || parts > kMaxParts) {
        log("NXPARTS_BROKEN " + name);
        return false;
    }

    // An earlier start may have been interrupted. Finished parts are already deleted, so the
    // parts still on disk must be one unbroken run that ends at the last index.
    std::vector<std::uint64_t> sizes; // remaining parts, in index order
    std::uint64_t first = parts;      // index of the first remaining part
    std::uint64_t remaining = 0;
    for (std::uint64_t index = 0; index < parts; ++index) {
        std::error_code error;
        const fs::path part = PartPath(target, index);
        if (fs::exists(part, error) && !error) {
            first = std::min(first, index);
            sizes.push_back(SizeOrZero(part));
            remaining += sizes.back();
        } else if (first != parts) {
            log("NXPARTS_BROKEN " + name + " missing part " + std::to_string(index));
            return false;
        }
    }
    if (remaining > size) {
        log("NXPARTS_BROKEN " + name);
        return false;
    }

    // The target must already hold everything except the parts still waiting.
    std::uint64_t expected = size - remaining;
    std::error_code error;
    const bool exists = fs::exists(target, error);
    std::uint64_t current = exists ? SizeOrZero(target) : 0;
    if (current > expected) {
        fs::resize_file(target, expected); // an append was interrupted
        current = expected;
    }
    if (current < expected) {
        log("NXPARTS_BROKEN " + name);
        return false;
    }
    if (!exists) {
        std::ofstream(target, std::ios::binary).close();
    }

    for (std::uint64_t index = first; index < parts; ++index) {
        const std::uint64_t part_size = sizes[index - first];
        // A failed space query is not a reason to give up; a failed write is caught below.
        const auto space = fs::space(target.parent_path(), error);
        if (!error && space.available < part_size) {
            log("NXPARTS_NO_SPACE " + name + " need=" + std::to_string(part_size >> 20) +
                " free=" + std::to_string(space.available >> 20));
            return false;
        }
        const fs::path part = PartPath(target, index);
        Append(target, part);
        expected += part_size;
        if (SizeOrZero(target) != expected) {
            throw std::runtime_error("short append of " + Utf8(part.filename()));
        }
        fs::remove(part);
    }

    fs::remove(manifest);
    log("NXPARTS_JOINED " + name + " " + std::to_string(size >> 20));
    return true;
}
} // namespace detail

// Joins every split upload under `folder` (subfolders included). Safe to call on every start and
// from several threads; it never throws and resumes whatever an earlier start left half done.
inline JoinResult JoinSplitUploads(const std::filesystem::path& folder, const Log& log_to) {
    namespace fs = std::filesystem;
    static std::mutex join_mutex;
    const std::lock_guard<std::mutex> lock(join_mutex);
    const Log log = [&log_to](const std::string& line) {
        if (log_to) {
            log_to(line);
        }
    };
    JoinResult result;
    try {
        std::vector<fs::path> manifests;
        std::error_code error;
        fs::recursive_directory_iterator iterator(
            folder, fs::directory_options::skip_permission_denied, error);
        for (; !error && iterator != fs::recursive_directory_iterator();
             iterator.increment(error)) {
            std::error_code entry_error;
            if (iterator->is_regular_file(entry_error) &&
                iterator->path().extension() == ".nxparts") {
                manifests.push_back(iterator->path());
            }
        }
        std::sort(manifests.begin(), manifests.end());
        for (const auto& manifest : manifests) {
            try {
                ++(detail::JoinOne(manifest, log) ? result.joined : result.skipped);
            } catch (const std::exception& e) {
                log("NXPARTS_ERROR " + detail::Utf8(manifest.filename()) + " " + e.what());
                ++result.skipped;
            }
        }
    } catch (const std::exception& e) {
        log(std::string("NXPARTS_ERROR ") + e.what());
    }
    return result;
}
} // namespace SplitUpload
