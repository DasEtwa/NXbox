// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Trusted, local executable cache. No pointers or compiler-dependent object layouts on disk.
namespace NxboxJitCache {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t FormatVersion = 1;
constexpr std::size_t TitleLimit = 16 * 1024 * 1024;
constexpr std::size_t TotalLimit = 128 * 1024 * 1024;
constexpr std::size_t GuestLimit = 4096;
constexpr std::size_t HostLimit = 65536;

inline std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (auto byte : bytes) {
        value = (value ^ byte) * 1099511628211ULL;
    }
    return value;
}
inline void Put(Bytes& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}
inline void PutBlob(Bytes& bytes, std::span<const std::uint8_t> value) {
    Put(bytes, value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
}
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes_) : bytes(bytes_) {}
    bool Get(std::uint64_t& value) {
        if (bytes.size() < 8) {
            return false;
        }
        value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= std::uint64_t{bytes[i]} << (8 * i);
        }
        bytes = bytes.subspan(8);
        return true;
    }
    bool Blob(Bytes& value, std::size_t limit) {
        std::uint64_t size;
        if (!Get(size) || size > limit || size > bytes.size()) {
            return false;
        }
        value.assign(bytes.begin(), bytes.begin() + size);
        bytes = bytes.subspan(size);
        return true;
    }
    bool Empty() const {
        return bytes.empty();
    }

private:
    std::span<const std::uint8_t> bytes;
};

// Relocate the terminal by re-emitting it, including fresh patch_information entries.
// Copying old linked terminal bytes would resurrect dead cross-block pointers.
enum class Terminal : std::uint64_t { Dispatch, Link, LinkFast, PopRSB, FastDispatch };
struct Relocation {
    std::uint64_t offset = 0;
    Terminal terminal = Terminal::Dispatch;
    std::uint64_t target = 0;
};
struct Record {
    std::uint64_t descriptor = 0;
    std::uint64_t pc = 0;
    Bytes config;
    Bytes guest;
    Bytes host;
    Relocation relocation;

    bool Valid() const {
        return !config.empty() && config.size() <= 4096 && !guest.empty() &&
               guest.size() <= GuestLimit && guest.size() % 4 == 0 && pc % 4 == 0 &&
               pc <= std::numeric_limits<std::uint64_t>::max() - guest.size() && !host.empty() &&
               host.size() <= HostLimit && relocation.offset == host.size() &&
               static_cast<std::uint64_t>(relocation.terminal) <=
                   static_cast<std::uint64_t>(Terminal::FastDispatch) &&
               ((relocation.terminal == Terminal::Link ||
                 relocation.terminal == Terminal::LinkFast) ||
                relocation.target == 0);
    }
    bool Matches(std::span<const std::uint8_t> current) const {
        // Hash is an index/check, never the final authority for guest identity.
        return current.size() == guest.size() && Hash(current) == Hash(guest) &&
               std::equal(current.begin(), current.end(), guest.begin());
    }
};
inline Bytes EncodeRecord(const Record& record) {
    Bytes out;
    Put(out, record.descriptor);
    Put(out, record.pc);
    PutBlob(out, record.config);
    PutBlob(out, record.guest);
    Put(out, Hash(record.guest));
    PutBlob(out, record.host);
    Put(out, record.relocation.offset);
    Put(out, static_cast<std::uint64_t>(record.relocation.terminal));
    Put(out, record.relocation.target);
    return out;
}
inline bool DecodeRecord(std::span<const std::uint8_t> bytes, Record& record) {
    Reader reader(bytes);
    std::uint64_t hash, terminal;
    if (!reader.Get(record.descriptor) || !reader.Get(record.pc) ||
        !reader.Blob(record.config, 4096) || !reader.Blob(record.guest, GuestLimit) ||
        !reader.Get(hash) || !reader.Blob(record.host, HostLimit) ||
        !reader.Get(record.relocation.offset) || !reader.Get(terminal) ||
        !reader.Get(record.relocation.target) || !reader.Empty()) {
        return false;
    }
    record.relocation.terminal = static_cast<Terminal>(terminal);
    return record.Valid() && hash == Hash(record.guest);
}

// Entry guards use this allocation-free check even when a link/RSB bypasses GetBlock.
template <typename ReadWord>
inline bool MatchesGuest(const Record& record, ReadWord read_word) {
    if (record.guest.empty() || record.guest.size() % 4 != 0 ||
        record.pc > std::numeric_limits<std::uint64_t>::max() - record.guest.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset < record.guest.size(); offset += 4) {
        const auto current = read_word(record.pc + offset);
        if (!current) {
            return false;
        }
        std::uint32_t expected = 0;
        for (unsigned i = 0; i < 4; ++i) {
            expected |= std::uint32_t{record.guest[offset + i]} << (i * 8);
        }
        if (*current != expected) {
            return false;
        }
    }
    return true;
}

// Validate relocation metadata before invoking either writer. Terminal byte emission uses
// the current code address and runtime symbols, not a guessed address delta.
template <typename CopyBody, typename EmitTerminal>
inline bool Relocate(const Record& record, CopyBody copy_body, EmitTerminal emit_terminal) {
    if (!record.Valid()) {
        return false;
    }
    copy_body(record.host);
    emit_terminal(record.relocation);
    return true;
}

class Store {
public:
    using Report = std::function<void(const std::string&)>;
    Store(std::filesystem::path path_, std::uint64_t title_, Report report_)
        : path(std::move(path_)), title(title_), report(std::move(report_)) {}
    std::uint64_t Title() const {
        return title;
    }
    // CPU interfaces register their exact active keys before the frontend opens the file.
    bool RegisterConfig(const Bytes& config) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (config.empty()) {
                return false;
            }
            active_configs.insert(config);
            return true;
        } catch (...) {
            return false;
        }
    }
    // Called by frontend after loading updated executable metadata, before CPUs start.
    void Open(Bytes identity_) noexcept {
        try {
            std::lock_guard lock(mutex);
            identity = std::move(identity_);
            ready = !identity.empty();
            records.clear();
            used = 0;
            dirty = false;
            if (!ready) {
                Log("disabled_no_build_id");
                return;
            }
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (!ec && size <= TitleLimit) {
                std::ifstream input(path, std::ios::binary);
                Bytes file(size);
                if (input.read(reinterpret_cast<char*>(file.data()), file.size())) {
                    Decode(file);
                }
            }
            if (!active_configs.empty()) {
                for (auto it = records.begin(); it != records.end();) {
                    if (!active_configs.contains(it->second.config)) {
                        used -= EncodeRecord(it->second).size() + 16;
                        it = records.erase(it);
                        dirty = true;
                    } else {
                        ++it;
                    }
                }
            }
            Log("load");
        } catch (...) {
            ready = false;
        }
    }
    std::optional<Record> Find(std::uint64_t descriptor, const Bytes& config) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (!ready) {
                return std::nullopt;
            }
            const auto it = records.find({descriptor, config});
            if (it != records.end()) {
                return it->second;
            }
            ++misses;
        } catch (...) {
        }
        return std::nullopt;
    }
    void Result(bool hit, std::size_t bytes = 0) noexcept {
        std::lock_guard lock(mutex);
        if (hit) {
            ++hits;
            loaded_bytes += bytes;
        } else {
            ++misses;
        }
    }
    void Insert(Record record) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (!ready || !record.Valid()) {
                return;
            }
            const Key key{record.descriptor, record.config};
            const auto size = EncodeRecord(record).size() + 16;
            const auto it = records.find(key);
            const auto old_size = it == records.end() ? 0 : EncodeRecord(it->second).size() + 16;
            if (used - old_size + size > TitleLimit - 8192) {
                return;
            }
            used = used - old_size + size;
            records.insert_or_assign(key, std::move(record));
            dirty = true;
            if (std::chrono::steady_clock::now() - last_flush >= std::chrono::seconds(30)) {
                FlushLocked();
            }
        } catch (...) {
        }
    }
    void Flush() noexcept {
        try {
            std::lock_guard lock(mutex);
            FlushLocked();
            Log("exit");
        } catch (...) {
        }
    }

private:
    using Key = std::pair<std::uint64_t, Bytes>;
    void Log(const char* phase) {
        if (report) {
            report("JIT_CACHE phase=" + std::string(phase) + " hits=" + std::to_string(hits) +
                   " misses=" + std::to_string(misses) + " bytes=" + std::to_string(loaded_bytes) +
                   " stored_bytes=" + std::to_string(used) +
                   " blocks=" + std::to_string(records.size()));
        }
    }
    void Decode(const Bytes& file) {
        Reader reader(file);
        std::uint64_t magic, version, count;
        Bytes saved_identity;
        if (!reader.Get(magic) || magic != 0x314354494A584EULL || !reader.Get(version) ||
            version != FormatVersion || !reader.Blob(saved_identity, 4096) ||
            saved_identity != identity || !reader.Get(count) || count > TitleLimit / 96) {
            return;
        }
        std::map<Key, Record> parsed;
        std::size_t parsed_size = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            Bytes blob;
            std::uint64_t checksum;
            Record record;
            if (!reader.Blob(blob, HostLimit + GuestLimit + 8192) || !reader.Get(checksum) ||
                checksum != Hash(blob) || !DecodeRecord(blob, record) ||
                !parsed.emplace(Key{record.descriptor, record.config}, record).second) {
                return;
            }
            parsed_size += blob.size() + 16;
        }
        if (!reader.Empty()) {
            return;
        }
        records = std::move(parsed);
        used = parsed_size;
    }
    void FlushLocked() {
        last_flush = std::chrono::steady_clock::now();
        if (!ready || !dirty) {
            return;
        }
        Bytes file;
        Put(file, 0x314354494A584EULL);
        Put(file, FormatVersion);
        PutBlob(file, identity);
        Put(file, records.size());
        for (const auto& [key, record] : records) {
            const auto blob = EncodeRecord(record);
            PutBlob(file, blob);
            Put(file, Hash(blob));
        }
        if (file.size() > TitleLimit) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return;
        }
        const auto temporary =
            std::filesystem::path(path.native() + std::filesystem::path(".tmp").native());
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.data()), file.size());
            output.flush();
            if (!output) {
                return;
            }
        }
        // Windows rename does not replace existing files. Remove only this disposable cache.
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            return;
        }
        dirty = false;
        Prune();
        Log("save");
    }
    void Prune() {
        std::vector<std::filesystem::directory_entry> files;
        std::uintmax_t total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                total += entry.file_size();
                files.push_back(entry);
            }
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return a.last_write_time() < b.last_write_time();
        });
        for (const auto& entry : files) {
            if (total <= TotalLimit) {
                break;
            }
            if (entry.path() != path) {
                const auto size = entry.file_size();
                std::error_code ec;
                if (std::filesystem::remove(entry.path(), ec)) {
                    total -= size;
                }
            }
        }
    }
    std::filesystem::path path;
    std::uint64_t title;
    Report report;
    std::mutex mutex;
    Bytes identity;
    std::map<Key, Record> records;
    std::set<Bytes> active_configs;
    std::size_t used = 0;
    std::uint64_t hits = 0, misses = 0, loaded_bytes = 0;
    bool ready = false, dirty = false;
    std::chrono::steady_clock::time_point last_flush = std::chrono::steady_clock::now();
};
// Frontend writes only before constructing CPU interfaces and after all CPU threads stop.
inline std::shared_ptr<Store> session;
} // namespace NxboxJitCache
