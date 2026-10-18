// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend_common/firmware_sources.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <zlib.h>

namespace FirmwareManager::Sources {
namespace {
using U64 = std::uint64_t;
constexpr U64 Limit = 8ull << 30;
[[noreturn]] void Invalid() {
    throw std::runtime_error("Invalid firmware source");
}
void Require(bool ok) {
    if (!ok)
        Invalid();
}
std::string PathText(const fs::path& p) {
    const auto text = p.generic_u8string();
    return {text.begin(), text.end()};
}
fs::path Utf8Path(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}
bool Ignored(const fs::path& p) {
    for (const auto& part : p) {
        const auto name = PathText(part);
        if (name.starts_with("._") || name == "__MACOSX")
            return true;
    }
    return false;
}
std::string Lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Utf8(std::u16string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned c = text[i];
        if (c >= 0xd800 && c <= 0xdbff) {
            Require(i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff);
            c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
        } else
            Require(c < 0xdc00 || c > 0xdfff);
        if (c < 0x80)
            out += static_cast<char>(c);
        else {
            if (c >= 0x10000)
                out += static_cast<char>(0xf0 | (c >> 18));
            else if (c >= 0x800)
                out += static_cast<char>(0xe0 | (c >> 12));
            else
                out += static_cast<char>(0xc0 | (c >> 6));
            if (c >= 0x10000)
                out += static_cast<char>(0x80 | ((c >> 12) & 63));
            if (c >= 0x800)
                out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    return out;
}
bool Nca(const fs::path& p) {
    return !Ignored(p) && Lower(PathText(p.extension())) == ".nca";
}
bool Part(const std::string& s) {
    return s.size() == 2 && s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9';
}
U64 Le(View b, size_t p, size_t n) {
    Require(p <= b.size() && n <= b.size() - p && n <= 8);
    U64 v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= U64(b[p + i]) << (8 * i);
    return v;
}
std::vector<std::uint8_t> Read(const Reader& r, U64 p, size_t n) {
    std::vector<std::uint8_t> b(n);
    r.Read(p, b);
    return b;
}
void Copy(const Reader& r, Writer& w, U64 offset, U64 size) {
    std::array<std::uint8_t, 65536> b;
    while (size) {
        const auto n = static_cast<size_t>(std::min<U64>(size, b.size()));
        r.Read(offset, Bytes(b).first(n));
        w.Write(View(b).first(n));
        offset += n;
        size -= n;
    }
}
std::vector<Entry> Ordered(std::vector<Entry> parts, bool raw) {
    std::sort(parts.begin(), parts.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });
    Require(!parts.empty());
    if (raw && Lower(PathText(parts[0].path.filename())) == "rawnand.bin") {
        Require(parts.size() == 1 && parts[0].size);
        return parts;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto name = PathText(parts[i].path.filename());
        const auto suffix = raw ? name.substr(name.size() - 2) : name;
        Require(Part(suffix) && std::stoul(suffix) == i && parts[i].size);
    }
    return parts;
}
void Log(const IO& io, const std::string& s) {
    if (io.log)
        io.log("FIRMWARE " + s);
}
// Called only from an active catch; exception matching works with the project's disabled RTTI.
bool AccessDenied() {
    try {
        throw;
    } catch (const std::system_error& error) {
        return error.code() == std::errc::permission_denied ||
               (error.code().category() == std::system_category() && error.code().value() == 5);
    } catch (...) {
        return false;
    }
}
struct Output {
    const IO& io;
    fs::path temporary;
    std::set<std::string> names;
    std::vector<fs::path> files;
    U64 total{};
    bool created{};
    std::unique_ptr<Writer> New(const std::string& name, U64 size) {
        Require(size && size <= Limit - total && names.insert(Lower(name)).second);
        Require(Utf8Path(name).filename() == Utf8Path(name) && Nca(Utf8Path(name)));
        total += size;
        if (!created) {
            io.mkdir(temporary);
            created = true;
        }
        auto path = temporary / Utf8Path(name);
        auto writer = io.create(path);
        files.push_back(path);
        return writer;
    }
};

// ZIP central-directory sizes are authoritative, including archives with data descriptors.
struct ZipEntry {
    std::string name;
    U64 size, packed, offset;
    unsigned method;
    std::uint32_t crc;
};
std::vector<ZipEntry> Zip(const Reader& r) {
    Require(r.size >= 22);
    const auto tail_size = static_cast<size_t>(std::min<U64>(r.size, 65557));
    const auto tail = Read(r, r.size - tail_size, tail_size);
    size_t end = tail.size() - 22;
    for (;;) {
        if (Le(tail, end, 4) == 0x06054b50 && end + 22 + Le(tail, end + 20, 2) == tail.size())
            break;
        Require(end != 0);
        --end;
    }
    Require(!Le(tail, end + 4, 2) && !Le(tail, end + 6, 2));
    U64 count = Le(tail, end + 10, 2), length = Le(tail, end + 12, 4), pos = Le(tail, end + 16, 4);
    if (count == 0xffff || length == 0xffffffff || pos == 0xffffffff) {
        const U64 eocd = r.size - tail_size + end;
        Require(eocd >= 20);
        const auto locator = Read(r, eocd - 20, 20);
        Require(Le(locator, 0, 4) == 0x07064b50 && !Le(locator, 4, 4) && Le(locator, 16, 4) == 1);
        const auto z = Read(r, Le(locator, 8, 8), 56);
        Require(Le(z, 0, 4) == 0x06064b50 && Le(z, 4, 8) >= 44 && !Le(z, 16, 4) && !Le(z, 20, 4));
        Require(Le(z, 24, 8) == Le(z, 32, 8));
        count = Le(z, 32, 8);
        length = Le(z, 40, 8);
        pos = Le(z, 48, 8);
    } else
        Require(Le(tail, end + 8, 2) == count);
    Require(count <= 100000 && pos <= r.size && length <= r.size - pos);
    const auto stop = pos + length;
    std::vector<ZipEntry> result;
    std::set<std::string> seen;
    U64 name_bytes = 0;
    for (U64 i = 0; i < count; ++i) {
        Require(pos <= stop && stop - pos >= 46);
        auto h = Read(r, pos, 46);
        Require(Le(h, 0, 4) == 0x02014b50);
        const size_t n = Le(h, 28, 2), x = Le(h, 30, 2), comment = Le(h, 32, 2);
        Require(46 + n + x + comment <= stop - pos);
        auto b = Read(r, pos + 46, n + x);
        std::string name(b.begin(), b.begin() + n);
        std::replace(name.begin(), name.end(), '\\', '/');
        ZipEntry e{name,
                   Le(h, 24, 4),
                   Le(h, 20, 4),
                   Le(h, 42, 4),
                   static_cast<unsigned>(Le(h, 10, 2)),
                   static_cast<std::uint32_t>(Le(h, 16, 4))};
        pos += 46 + n + x + comment;
        const fs::path entry_path = Utf8Path(name);
        if (Ignored(entry_path) || !(Nca(entry_path) || (Nca(entry_path.parent_path()) &&
                                                         Part(PathText(entry_path.filename())))))
            continue;
        Require(n <= 4096 && name_bytes + n <= (16ull << 20));
        name_bytes += n;
        Require(!(Le(h, 8, 2) & 1) && !Le(h, 34, 2));
        Require(!name.empty() && name.front() != '/' && name.find(':') == std::string::npos &&
                name.find('\0') == std::string::npos);
        for (const auto& component : entry_path)
            Require(component != "..");
        Require(seen.insert(Lower(name)).second);
        const auto unix_mode = Le(h, 38, 4) >> 16;
        Require((unix_mode & 0170000) != 0120000);
        bool needs64 = e.size == 0xffffffff || e.packed == 0xffffffff || e.offset == 0xffffffff;
        for (size_t p = n; p + 4 <= b.size();) {
            const auto tag = Le(b, p, 2), len = Le(b, p + 2, 2);
            p += 4;
            Require(len <= b.size() - p);
            if (tag == 1) {
                auto extra = View(b).subspan(p, len);
                size_t q = 0;
                if (e.size == 0xffffffff) {
                    e.size = Le(extra, q, 8);
                    q += 8;
                }
                if (e.packed == 0xffffffff) {
                    e.packed = Le(extra, q, 8);
                    q += 8;
                }
                if (e.offset == 0xffffffff) {
                    e.offset = Le(extra, q, 8);
                }
                needs64 = false;
            }
            p += len;
        }
        Require(!needs64 && e.offset <= r.size && e.packed <= r.size - e.offset);
        result.push_back(std::move(e));
    }
    return result;
}
void Unzip(const Reader& r, const ZipEntry& e, Writer& w) {
    Require(e.method == 0 || e.method == 8);
    auto h = Read(r, e.offset, 30);
    Require(Le(h, 0, 4) == 0x04034b50 && !(Le(h, 6, 2) & 1) && Le(h, 8, 2) == e.method);
    U64 pos = e.offset + 30 + Le(h, 26, 2) + Le(h, 28, 2), left = e.packed, written = 0;
    Require(pos <= r.size && left <= r.size - pos);
    std::array<std::uint8_t, 65536> in, out;
    auto crc = crc32(0, Z_NULL, 0);
    const auto emit = [&](View b) {
        Require(b.size() <= e.size - written);
        written += b.size();
        crc = crc32(crc, b.data(), static_cast<uInt>(b.size()));
        w.Write(b);
    };
    z_stream z{};
    if (e.method == 8)
        Require(inflateInit2(&z, -MAX_WBITS) == Z_OK);
    struct Guard {
        z_stream& z;
        bool active;
        ~Guard() {
            if (active)
                inflateEnd(&z);
        }
    } guard{z, e.method == 8};
    int status = Z_OK;
    while (left) {
        const auto n = static_cast<size_t>(std::min<U64>(left, in.size()));
        r.Read(pos, Bytes(in).first(n));
        pos += n;
        left -= n;
        if (!e.method) {
            emit(View(in).first(n));
            continue;
        }
        Require(status != Z_STREAM_END);
        z.next_in = in.data();
        z.avail_in = static_cast<uInt>(n);
        do {
            z.next_out = out.data();
            z.avail_out = static_cast<uInt>(out.size());
            const auto before = z.avail_in;
            status = inflate(&z, Z_NO_FLUSH);
            if (status == Z_BUF_ERROR && !z.avail_in)
                break;
            Require(status == Z_OK || status == Z_STREAM_END);
            const auto produced = out.size() - z.avail_out;
            emit(View(out).first(produced));
            if (status == Z_STREAM_END) {
                Require(!z.avail_in && !left);
                break;
            }
            Require(produced || z.avail_in < before);
        } while (z.avail_in || !z.avail_out);
    }
    Require((e.method == 0 || status == Z_STREAM_END) && written == e.size && crc == e.crc);
}
void ExtractZip(const Reader& r, Output& output, std::vector<std::vector<fs::path>>* sets) {
    auto entries = Zip(r);
    std::map<std::string, std::vector<ZipEntry>> groups;
    for (const auto& e : entries) {
        const fs::path p = Utf8Path(e.name);
        if (e.name.back() == '/')
            continue;
        Require(std::distance(p.begin(), p.end()) <= 11);
        if (Nca(p))
            groups[e.name].push_back(e);
        else if (Nca(p.parent_path()) && Part(PathText(p.filename())))
            groups[PathText(p.parent_path())].push_back(e);
    }
    std::map<fs::path, std::unique_ptr<Output>> candidates;
    std::set<fs::path> invalid;
    for (auto& [name, parts] : groups) {
        const auto directory = Utf8Path(name).parent_path();
        if (invalid.contains(directory))
            continue;
        try {
            std::sort(parts.begin(), parts.end(),
                      [](const auto& a, const auto& b) { return a.name < b.name; });
            U64 size = 0;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (parts[i].name != name)
                    Require(Part(PathText(Utf8Path(parts[i].name).filename())) &&
                            std::stoul(PathText(Utf8Path(parts[i].name).filename())) == i);
                Require(parts[i].size <= Limit - size);
                size += parts[i].size;
            }
            auto& candidate = candidates[directory];
            if (!candidate) {
                if (!output.created) {
                    output.io.mkdir(output.temporary);
                    output.created = true;
                }
                candidate = std::make_unique<Output>(Output{
                    output.io, output.temporary / std::to_string(candidates.size()), {}, {}, 0});
            }
            auto w = candidate->New(PathText(Utf8Path(name).filename()), size);
            for (const auto& e : parts)
                Unzip(r, e, *w);
            w->Finish();
        } catch (const std::exception& error) {
            if (!sets)
                throw;
            invalid.insert(directory);
            const auto found = candidates.find(directory);
            if (found != candidates.end() && found->second && found->second->created)
                output.io.remove_tree(found->second->temporary);
            Log(output.io, std::string("candidate_extraction_failed ") + error.what());
        }
    }
    for (const auto& [directory, candidate] : candidates) {
        if (invalid.contains(directory))
            continue;
        output.files.insert(output.files.end(), candidate->files.begin(), candidate->files.end());
        if (sets)
            sets->push_back(candidate->files);
    }
    if (groups.empty())
        Log(output.io, "zip_no_nca ignored");
}

struct FatEntry {
    std::string name;
    std::uint32_t cluster, size;
    bool directory;
};
class Fat {
    Reader r;
    U64 fat{}, data{}, clusters{}, fat_size{};
    unsigned cluster_size{}, root{};
    template <typename F>
    void Chain(unsigned c, F use) {
        U64 hops = 0;
        while (c < 0xffffff8) {
            Require(c >= 2 && c < clusters + 2 && c * 4ull < fat_size && ++hops <= clusters);
            if (!use(Read(r, data + (c - 2ull) * cluster_size, cluster_size)))
                return;
            c = static_cast<unsigned>(Le(Read(r, fat + c * 4ull, 4), 0, 4)) & 0xfffffff;
        }
    }

public:
    explicit Fat(Reader reader) : r(std::move(reader)) {
        const auto b = Read(r, 0, 512);
        const U64 sector = Le(b, 11, 2), spc = b[13], reserved = Le(b, 14, 2), fats = b[16],
                  fs = Le(b, 36, 4), total = Le(b, 32, 4);
        Require(Le(b, 510, 2) == 0xaa55 &&
                (sector == 512 || sector == 1024 || sector == 2048 || sector == 4096) && spc &&
                !(spc & (spc - 1)) && spc <= 128 && reserved && fats && fats <= 2 && fs &&
                !Le(b, 17, 2) && !Le(b, 22, 2) && total * sector <= r.size);
        data = (reserved + fats * fs) * sector;
        cluster_size = static_cast<unsigned>(sector * spc);
        Require(data < total * sector);
        clusters = (total * sector - data) / cluster_size;
        fat_size = fs * sector;
        Require(clusters && (clusters + 2) * 4 <= fat_size);
        const auto flags = Le(b, 40, 2), active = flags & 0x80 ? flags & 15 : 0;
        Require(active < fats);
        fat = (reserved + active * fs) * sector;
        root = static_cast<unsigned>(Le(b, 44, 4));
    }
    std::vector<FatEntry> Directory(unsigned c) {
        std::vector<FatEntry> entries;
        std::map<unsigned, std::u16string> long_name;
        Chain(c, [&](const auto& b) {
            for (size_t p = 0; p < b.size(); p += 32) {
                auto e = View(b).subspan(p, 32);
                if (!e[0])
                    return false;
                if (e[0] == 0xe5) {
                    long_name.clear();
                    continue;
                }
                if (e[11] == 15) {
                    const auto n = e[0] & 31;
                    Require(n && n <= 20);
                    if (e[0] & 0x40)
                        long_name.clear();
                    std::u16string text;
                    for (size_t i : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
                        const auto ch = Le(e, i, 2);
                        if (!ch || ch == 0xffff)
                            break;
                        text += static_cast<char16_t>(ch);
                    }
                    long_name[n] = text;
                    continue;
                }
                auto trim = [](std::string s) {
                    while (!s.empty() && s.back() == ' ')
                        s.pop_back();
                    return s;
                };
                std::string name = trim(std::string(e.begin(), e.begin() + 8));
                auto ext = trim(std::string(e.begin() + 8, e.begin() + 11));
                if (!ext.empty())
                    name += "." + ext;
                if (!long_name.empty()) {
                    std::u16string joined;
                    for (auto& [n, s] : long_name)
                        joined += s;
                    name = Utf8(joined);
                }
                long_name.clear();
                if ((e[11] & 8) || name == "." || name == "..")
                    continue;
                Require(!name.empty() && name.find_first_of("/\\:") == std::string::npos &&
                        name.find('\0') == std::string::npos);
                entries.push_back({name, static_cast<unsigned>((Le(e, 20, 2) << 16) | Le(e, 26, 2)),
                                   static_cast<unsigned>(Le(e, 28, 4)), bool(e[11] & 16)});
                Require(entries.size() <= 100000);
            }
            return true;
        });
        return entries;
    }
    unsigned Child(unsigned c, const std::string& name) {
        unsigned result = 0;
        for (const auto& e : Directory(c))
            if (Lower(e.name) == name && e.directory) {
                Require(!result);
                result = e.cluster;
            }
        Require(result != 0);
        return result;
    }
    void File(const FatEntry& e, Writer& w) {
        U64 left = e.size;
        Require(left && !e.directory);
        Chain(e.cluster, [&](const auto& b) {
            const auto n = static_cast<size_t>(std::min<U64>(left, b.size()));
            w.Write(View(b).first(n));
            left -= n;
            return left != 0;
        });
        Require(!left);
    }
    void Extract(Output& out) {
        std::set<unsigned> visited;
        const auto walk = [&](auto&& self, unsigned c, unsigned depth) -> void {
            Require(depth <= 8 && visited.insert(c).second);
            for (const auto& e : Directory(c)) {
                if (Ignored(Utf8Path(e.name)))
                    continue;
                if (Nca(Utf8Path(e.name))) {
                    if (!e.directory) {
                        auto w = out.New(e.name, e.size);
                        File(e, *w);
                        w->Finish();
                    } else {
                        auto parts = Directory(e.cluster);
                        std::sort(parts.begin(), parts.end(),
                                  [](const auto& a, const auto& b) { return a.name < b.name; });
                        U64 size = 0;
                        Require(!parts.empty());
                        for (size_t i = 0; i < parts.size(); ++i) {
                            Require(!parts[i].directory && Part(parts[i].name) &&
                                    std::stoul(parts[i].name) == i);
                            size += parts[i].size;
                        }
                        auto w = out.New(e.name, size);
                        for (const auto& part : parts)
                            File(part, *w);
                        w->Finish();
                    }
                } else if (e.directory)
                    self(self, e.cluster, depth + 1);
            }
        };
        walk(walk, Child(Child(root, "contents"), "registered"), 0);
    }
};
void ExtractNand(const Reader& nand, Output& out, const Decrypt& decrypt) {
    Require(bool(decrypt));
    auto h = Read(nand, 512, 512);
    Require(std::string(h.begin(), h.begin() + 8) == "EFI PART" && Le(h, 12, 4) >= 92);
    const U64 table_lba = Le(h, 72, 8), count = Le(h, 80, 4), stride = Le(h, 84, 4);
    Require(count && count <= 4096 && stride >= 128 && stride <= 4096 &&
            table_lba <= nand.size / 512);
    U64 offset = 0, size = 0;
    for (U64 i = 0; i < count; ++i) {
        auto e = Read(nand, table_lba * 512 + i * stride, 128);
        std::string name;
        for (size_t p = 56; p < 128; p += 2) {
            const auto c = Le(e, p, 2);
            if (!c)
                break;
            name += c < 128 ? static_cast<char>(c) : '?';
        }
        if (name != "SYSTEM")
            continue;
        Require(!size);
        const auto first = Le(e, 32, 8), last = Le(e, 40, 8);
        Require(first <= last && last < nand.size / 512);
        offset = first * 512;
        size = (last - first + 1) * 512;
        Require(size <= Limit && size % 0x4000 == 0);
    }
    Require(size != 0);
    struct Cache {
        U64 sector = std::numeric_limits<U64>::max();
        std::array<std::uint8_t, 0x4000> data;
    };
    auto cache = std::make_shared<Cache>();
    Reader plain{size, [=](U64 p, Bytes b) {
                     while (!b.empty()) {
                         const U64 sector = p / 0x4000;
                         const size_t inside = p % 0x4000;
                         if (cache->sector != sector) {
                             auto encrypted = Read(nand, offset + sector * 0x4000, 0x4000);
                             decrypt(sector, encrypted, cache->data);
                             cache->sector = sector;
                         }
                         const auto n = std::min(b.size(), cache->data.size() - inside);
                         std::copy_n(cache->data.begin() + inside, n, b.begin());
                         b = b.subspan(n);
                         p += n;
                     }
                 }};
    Fat(std::move(plain)).Extract(out);
}
} // namespace

void Reader::Read(U64 offset, Bytes bytes) const {
    Require(offset <= size && bytes.size() <= size - offset);
    read(offset, bytes);
}
Reader Joined(const IO& io, const std::vector<Entry>& parts) {
    auto readers = std::make_shared<std::vector<Reader>>();
    U64 size = 0;
    for (const auto& part : parts) {
        auto r = io.open(part.path);
        Require(r.size == part.size && r.size <= std::numeric_limits<U64>::max() - size);
        size += r.size;
        readers->push_back(std::move(r));
    }
    return {size, [readers](U64 p, Bytes b) {
                for (const auto& r : *readers) {
                    if (p >= r.size) {
                        p -= r.size;
                        continue;
                    }
                    const auto n = static_cast<size_t>(std::min<U64>(b.size(), r.size - p));
                    r.Read(p, b.first(n));
                    b = b.subspan(n);
                    p = 0;
                    if (b.empty())
                        return;
                }
                Require(b.empty());
            }};
}
bool OlderVersion(const std::string& source, const std::string& installed) {
    const auto parse = [](const std::string& text) {
        std::array<unsigned, 4> version{};
        size_t component = 0;
        for (const unsigned char ch : text) {
            if (ch == '.') {
                if (++component == version.size())
                    break;
            } else if (ch >= '0' && ch <= '9') {
                Require(version[component] <= 100000);
                version[component] = version[component] * 10 + ch - '0';
            } else
                break;
        }
        return version;
    };
    return !installed.empty() && parse(source) < parse(installed);
}
std::string ScanRecords(const std::vector<Source>& sources) {
    std::string records;
    for (const auto& source : sources)
        if (!source.parts.empty())
            records += "BEGIN\n" + Fingerprint(source) + "END\n";
    return records;
}
void SweepTemporary(const IO& io, const fs::path& local_state) {
    std::vector<Entry> entries;
    try {
        entries = io.list(local_state);
    } catch (const std::exception& error) {
        Log(io, std::string("temporary_cleanup_failed ") + error.what());
        return;
    }
    for (const auto& entry : entries) {
        try {
            if (entry.directory && !entry.reparse &&
                PathText(entry.path.filename()).starts_with(".firmware_extract-"))
                io.remove_tree(entry.path);
        } catch (const std::exception& error) {
            Log(io, std::string("temporary_cleanup_failed ") + error.what());
        }
    }
}
std::string Fingerprint(const Source& source) {
    std::string result;
    for (const auto& e : source.parts)
        result += PathText(e.path) + "\t" + std::to_string(e.size) + "\t" +
                  std::to_string(e.modified) + "\n";
    return result;
}

std::vector<Source> Discover(const IO& io, const std::vector<fs::path>& folders,
                             const std::vector<fs::path>& roots) {
    std::vector<Source> result;
    std::set<fs::path> seen;
    std::vector<Entry> metadata;
    const auto scan = [&](auto&& self, const fs::path& path, bool firmware,
                          unsigned depth) -> bool {
        if (!seen.insert(path).second)
            return false;
        std::vector<Entry> entries;
        try {
            entries = io.list(path);
        } catch (const std::exception& e) {
            if (AccessDenied() && Lower(PathText(path.filename())) == "emmc")
                Log(io, "nand_access_denied");
            Log(io, std::string("scan_failed ") + e.what());
            return true; // An unreadable subtree may contain a NAND: never rename its parent.
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.path < b.path; });
        bool protected_source = false;
        for (const auto& e : entries) {
            try {
                if (e.reparse || Ignored(e.path))
                    continue;
                if (!e.directory && Nca(e.path))
                    metadata.push_back(e);
                else if (e.directory && Nca(e.path)) {
                    metadata.push_back(e);
                    try {
                        for (const auto& chunk : io.list(e.path))
                            if (!chunk.reparse && !chunk.directory)
                                metadata.push_back(chunk);
                    } catch (const std::exception& error) {
                        if (AccessDenied())
                            Log(io, "chunk_access_denied");
                        Log(io, std::string("scan_failed ") + error.what());
                    }
                }
            } catch (const std::exception& error) {
                Log(io, std::string("entry_skipped ") + error.what());
                protected_source = true;
            }
        }
        std::vector<Entry> raw, emu;
        for (const auto& e : entries) {
            try {
                if (e.reparse || e.directory || Ignored(e.path))
                    continue;
                const auto name = Lower(PathText(e.path.filename()));
                if (name == "rawnand.bin" ||
                    (name.size() == 14 && name.starts_with("rawnand.bin.") &&
                     Part(name.substr(12))))
                    raw.push_back(e);
                if (Lower(PathText(path.filename())) == "emmc" && Part(name))
                    emu.push_back(e);
                if (Lower(PathText(e.path.extension())) == ".zip")
                    result.push_back({Kind::Zip, e.path, {e}, {}});
                if (Lower(PathText(e.path.extension())) == ".7z" ||
                    Lower(PathText(e.path.extension())) == ".rar" ||
                    Lower(PathText(e.path.extension())) == ".xci" || name == "raw_based")
                    Log(io, "unsupported " + PathText(e.path) +
                                "; NxbHelper on a computer can convert it");
            } catch (const std::exception& error) {
                Log(io, std::string("entry_skipped ") + error.what());
                protected_source = true;
            }
        }
        if (Lower(PathText(path.filename())) == "emmc" && emu.empty())
            Log(io, "nand_no_visible_chunks");
        for (auto* parts : {&raw, &emu})
            if (!parts->empty()) {
                protected_source = true;
                try {
                    auto ordered = Ordered(*parts, parts == &raw);
                    result.push_back({Kind::Nand, ordered.front().path, std::move(ordered), {}});
                } catch (const std::exception&) {
                    Log(io, "nand_scan invalid split sequence " + PathText(path));
                }
            }
        for (const auto& e : entries) {
            try {
                if (!e.directory || e.reparse || Ignored(e.path) || Nca(e.path))
                    continue;
                const auto name = Lower(PathText(e.path.filename()));
                if (name.starts_with("firmware_installed") || name.starts_with(".firmware_extract"))
                    continue;
                if (name == "emummc" || name == "emmc" || (firmware && depth >= 8))
                    protected_source = true;
                if ((firmware && depth < 8) || name == "emummc" ||
                    (depth < 3 && (Lower(PathText(path.filename())) == "emummc" || name == "emmc")))
                    protected_source = self(self, e.path, firmware, depth + 1) || protected_source;
            } catch (const std::exception& error) {
                Log(io, std::string("entry_skipped ") + error.what());
                protected_source = true;
            }
        }
        return protected_source;
    };
    for (const auto& folder : folders) {
        const auto start = result.size();
        metadata.clear();
        const bool protect = scan(scan, folder, true, 0);
        const bool folder_source = !metadata.empty();
        if (folder_source)
            result.insert(result.begin() + start,
                          {Kind::Folder, folder, metadata, protect ? fs::path{} : folder});
        // ZIP sources inside an ordinary firmware folder preserve the existing rename behavior.
        if (!protect)
            for (size_t i = start + (folder_source ? 1 : 0); i < result.size(); ++i)
                if (result[i].kind == Kind::Zip)
                    result[i].archive_folder = folder;
    }
    for (const auto& root : roots) {
        for (const auto& path : {root / "NXbox", root}) {
            metadata.clear();
            scan(scan, path, false, 0);
            // Loose NCAs at these locations are accepted without renaming the drive/NXbox.
            std::vector<Entry> direct;
            for (const auto& e : metadata) {
                if (e.path.parent_path() == path ||
                    (e.path.parent_path().parent_path() == path && Nca(e.path.parent_path())))
                    direct.push_back(e);
            }
            if (!direct.empty())
                result.push_back({Kind::Folder, path, std::move(direct), {}, false});
        }
    }
    return result;
}
std::vector<fs::path> Extract(const IO& io, const Source& source, const fs::path& temporary,
                              const Decrypt& decrypt, std::vector<std::vector<fs::path>>* sets) {
    Output out{io, temporary, {}, {}, 0};
    if (source.kind == Kind::Zip) {
        Log(io, "zip_extract");
        ExtractZip(io.open(source.path), out, sets);
    } else if (source.kind == Kind::Nand) {
        Log(io, "nand_scan");
        try {
            ExtractNand(Joined(io, source.parts), out, decrypt);
        } catch (const std::exception&) {
            if (AccessDenied())
                Log(io, "nand_access_denied");
            throw;
        }
        if (sets && !out.files.empty())
            sets->push_back(out.files);
        Log(io, "nand_extract count=" + std::to_string(out.files.size()));
    } else {
        size_t candidate_index = 0;
        const auto walk = [&](auto&& self, const fs::path& path, unsigned depth) -> void {
            const auto candidate_path = temporary / std::to_string(++candidate_index);
            auto candidate_io = io;
            candidate_io.mkdir = [&](const auto& p) {
                if (!out.created) {
                    io.mkdir(temporary);
                    out.created = true;
                }
                io.mkdir(p);
            };
            Output local{candidate_io, candidate_path, {}, {}, 0};
            bool invalid = false;
            for (const auto& e : io.list(path)) {
                bool nca_entry = false;
                try {
                    if (e.reparse || Ignored(e.path))
                        continue;
                    nca_entry = Nca(e.path);
                    if (nca_entry) {
                        if (e.directory) {
                            std::vector<Entry> parts;
                            for (const auto& p : io.list(e.path)) {
                                if (Ignored(p.path))
                                    continue;
                                Require(!p.directory && !p.reparse);
                                parts.push_back(p);
                            }
                            if (parts.empty())
                                Log(io, "chunk_no_visible_parts");
                            auto r = Joined(io, Ordered(std::move(parts), false));
                            auto w = local.New(PathText(e.path.filename()), r.size);
                            Copy(r, *w, 0, r.size);
                            w->Finish();
                            Log(io, "chunk_extract");
                        } else {
                            // Probe loose NCA access too, before the VFS verification stage.
                            io.open(e.path);
                            Require(local.names.insert(Lower(PathText(e.path.filename()))).second);
                            local.files.push_back(e.path);
                        }
                    } else if (e.directory && source.recursive && depth < 8 &&
                               !Lower(PathText(e.path.filename()))
                                    .starts_with(".firmware_extract") &&
                               !Lower(PathText(e.path.filename()))
                                    .starts_with("firmware_installed"))
                        self(self, e.path, depth + 1);
                } catch (const std::exception& error) {
                    if (AccessDenied() && nca_entry)
                        Log(io, "chunk_access_denied");
                    Log(io, std::string("entry_skipped ") + error.what());
                    if (nca_entry)
                        invalid = true;
                    else if (e.directory)
                        Log(io, "candidate_extraction_failed");
                }
            }
            if (invalid) {
                Log(io, "candidate_extraction_failed");
                if (!sets)
                    throw std::runtime_error("Firmware candidate extraction failed");
                return;
            }
            if (!local.files.empty()) {
                if (sets)
                    sets->push_back(local.files);
                out.files.insert(out.files.end(), local.files.begin(), local.files.end());
            }
        };
        walk(walk, source.path, 0);
    }
    std::sort(out.files.begin(), out.files.end());
    return out.files;
}
} // namespace FirmwareManager::Sources
