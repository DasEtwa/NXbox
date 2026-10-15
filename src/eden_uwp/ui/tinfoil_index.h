// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure Tinfoil index parsing and traversal scheduling. Network requests belong to the caller.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdenXbox::Tinfoil {
inline constexpr std::size_t kMaxEntries = 5000;
inline constexpr std::size_t kMaxDepth = 3;    // root is depth zero
inline constexpr std::size_t kMaxIndexes = 64; // includes the root and failed requests

enum class Kind { Other, Game, Update, Dlc };
enum class Status { Ok, Unsupported, SourceError };
enum class Unsupported { None, Encrypted, Ftp, GoogleDrive, OneFichier };
using Headers = std::vector<std::pair<std::string, std::string>>;
struct File {
    std::string name;
    std::string url; // absolute, without the display-name fragment
    std::uint64_t size = 0;
    Kind kind = Kind::Other;
};
struct Index {
    Status status = Status::Unsupported;
    Unsupported unsupported = Unsupported::None;
    std::vector<std::string> locations;
    std::vector<File> files;
    std::vector<std::string> directories;
    Headers headers;
    std::string success;
    std::string error;
};

namespace detail {
inline std::string Trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
inline std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}
inline int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

inline std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 &&
            HexValue(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

inline bool IsHttpUrl(const std::string& url) {
    const auto lower = Lower(url.substr(0, 8));
    const std::size_t start = lower.rfind("https://", 0) == 0  ? 8
                              : lower.rfind("http://", 0) == 0 ? 7
                                                               : 0;
    return start != 0 && url.size() > start && url[start] != '/' && url[start] != '?' &&
           url[start] != '#' && std::none_of(url.begin(), url.end(), [](unsigned char c) {
               return c <= ' ' || c == 127;
           });
}

// Small strict JSON reader. Unknown fields are still validated; nesting is bounded.
struct Value {
    enum class Type { Null, String, Number, Object, Array, Boolean } type = Type::Null;
    std::string text;
    std::vector<std::pair<std::string, Value>> object;
    std::vector<Value> array;
    const Value& At(std::string_view key) const {
        for (auto it = object.rbegin(); it != object.rend(); ++it)
            if (it->first == key)
                return it->second;
        static const Value empty;
        return empty;
    }
    std::string String() const {
        return type == Type::String ? text : std::string{};
    }
};
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}
    bool Read(Value& value) {
        return ReadValue(value, 0) && (Space(), pos_ == text_.size());
    }

private:
    void Space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\r' || text_[pos_] == '\n'))
            ++pos_;
    }
    bool Take(char c) {
        Space();
        if (pos_ == text_.size() || text_[pos_] != c)
            return false;
        ++pos_;
        return true;
    }
    bool Hex(unsigned& code) {
        code = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == text_.size())
                return false;
            const int digit = HexValue(text_[pos_++]);
            if (digit < 0)
                return false;
            code = code * 16 + static_cast<unsigned>(digit);
        }
        return true;
    }
    static void Utf8(std::string& out, unsigned code) {
        if (code <= 0x7F)
            out.push_back(static_cast<char>(code));
        else {
            if (code > 0xFFFF)
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            else if (code > 0x7FF)
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            else
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            if (code > 0xFFFF)
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 63)));
            if (code > 0x7FF)
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (code & 63)));
        }
    }
    bool String(std::string& out) {
        if (!Take('"'))
            return false;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"')
                return true;
            if (c < 32)
                return false;
            if (c != '\\') {
                if (c < 128) {
                    out.push_back(static_cast<char>(c));
                    continue;
                }
                const unsigned remaining = c >= 0xC2 && c <= 0xDF   ? 1
                                           : c >= 0xE0 && c <= 0xEF ? 2
                                           : c >= 0xF0 && c <= 0xF4 ? 3
                                                                    : 0;
                if (remaining == 0 || pos_ + remaining > text_.size())
                    return false;
                unsigned code = c & (remaining == 1 ? 31 : remaining == 2 ? 15 : 7);
                for (unsigned i = 0; i < remaining; ++i) {
                    const auto byte = static_cast<unsigned char>(text_[pos_++]);
                    if ((byte & 0xC0) != 0x80)
                        return false;
                    code = (code << 6) | (byte & 63);
                }
                if ((remaining == 2 && code < 0x800) || (remaining == 3 && code < 0x10000) ||
                    code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                    return false;
                Utf8(out, code);
                continue;
            }
            if (pos_ == text_.size())
                return false;
            const char escape = text_[pos_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned code;
                if (!Hex(code))
                    return false;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (pos_ + 2 > text_.size() || text_.substr(pos_, 2) != "\\u")
                        return false;
                    pos_ += 2;
                    unsigned low;
                    if (!Hex(low) || low < 0xDC00 || low > 0xDFFF)
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                } else if (code >= 0xDC00 && code <= 0xDFFF)
                    return false;
                Utf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool Digits() {
        const auto start = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9')
            ++pos_;
        return pos_ > start;
    }
    bool ReadValue(Value& value, unsigned depth) {
        Space();
        if (depth > 64 || pos_ == text_.size())
            return false;
        const char c = text_[pos_];
        if (c == '"') {
            value.type = Value::Type::String;
            return String(value.text);
        }
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            value.type = object ? Value::Type::Object : Value::Type::Array;
            ++pos_;
            if (Take(object ? '}' : ']'))
                return true;
            do {
                std::string key;
                if (object && (!String(key) || !Take(':')))
                    return false;
                Value child;
                if (!ReadValue(child, depth + 1))
                    return false;
                if (object)
                    value.object.emplace_back(std::move(key), std::move(child));
                else
                    value.array.push_back(std::move(child));
                if (Take(object ? '}' : ']'))
                    return true;
            } while (Take(','));
            return false;
        }
        for (const auto literal : {"null", "true", "false"}) {
            const std::string_view word(literal);
            if (text_.substr(pos_, word.size()) == word) {
                pos_ += word.size();
                value.type = word == "null" ? Value::Type::Null : Value::Type::Boolean;
                return true;
            }
        }
        const auto start = pos_;
        if (c == '-')
            ++pos_;
        if (pos_ == text_.size())
            return false;
        if (text_[pos_] == '0')
            ++pos_;
        else if (text_[pos_] < '1' || text_[pos_] > '9' || !Digits())
            return false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!Digits())
                return false;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (!Digits())
                return false;
        }
        value.type = Value::Type::Number;
        value.text = std::string(text_.substr(start, pos_ - start));
        return true;
    }
    std::string_view text_;
    std::size_t pos_ = 0;
};

inline std::uint64_t Size(const Value& value) {
    if (value.type != Value::Type::Number && value.type != Value::Type::String)
        return 0;
    const std::string text = Trim(value.text);
    if (text.empty())
        return 0;
    // Preserve all 64 bits for integer sizes, including numbers above double precision.
    if (std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        std::uint64_t size = 0;
        for (char c : text) {
            const auto digit = static_cast<unsigned>(c - '0');
            if (size > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
                return 0;
            size = size * 10 + digit;
        }
        return size;
    }
    // Numeric strings retain the previous atof-style prefix parsing.
    const long double size = std::strtold(text.c_str(), nullptr);
    return std::isfinite(size) && size > 0 && size < std::ldexp(1.0L, 64)
               ? static_cast<std::uint64_t>(size)
               : 0;
}
inline void AddHeader(Headers& headers, const std::string& name, const std::string& value) {
    // Refuse invalid field names and line breaks before passing data to an HTTP API.
    if (name.empty() ||
        std::any_of(name.begin(), name.end(),
                    [](unsigned char c) {
                        return !(std::isalnum(c) ||
                                 std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) !=
                                     std::string_view::npos);
                    }) ||
        value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos)
        return;
    headers.emplace_back(name, value);
}
inline void ReadHeaders(const Value& value, Headers& headers) {
    if (value.type == Value::Type::Object) {
        if (value.At("name").type == Value::Type::String &&
            value.At("value").type == Value::Type::String)
            AddHeader(headers, value.At("name").text, value.At("value").text);
        else
            for (const auto& item : value.object)
                if (item.second.type == Value::Type::String)
                    AddHeader(headers, item.first, item.second.text);
    } else if (value.type == Value::Type::Array) {
        for (const auto& item : value.array) {
            if (item.type == Value::Type::String) {
                const auto colon = item.text.find(':');
                if (colon != std::string::npos)
                    AddHeader(headers, Trim(item.text.substr(0, colon)),
                              Trim(item.text.substr(colon + 1)));
            } else if (item.type == Value::Type::Array && item.array.size() == 2 &&
                       item.array[0].type == Value::Type::String &&
                       item.array[1].type == Value::Type::String)
                AddHeader(headers, item.array[0].text, item.array[1].text);
            else
                ReadHeaders(item, headers);
        }
    }
}
} // namespace detail

// Resolves HTTP references, including root-relative paths, queries, and dot segments.
inline std::optional<std::string> Resolve(const std::string& base, const std::string& reference) {
    std::string ref = detail::Trim(reference);
    if (ref.empty())
        return std::nullopt;
    if (detail::IsHttpUrl(ref))
        return ref;
    if (!detail::IsHttpUrl(base))
        return std::nullopt;
    // WinRT Uri encoded spaces and UTF-8 bytes in relative references.
    std::string encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : ref) {
        if (c == ' ' || c >= 128) {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        } else
            encoded += static_cast<char>(c);
    }
    ref = std::move(encoded);
    const auto scheme = base.find("://");
    if (ref.rfind("//", 0) == 0) {
        const auto absolute = base.substr(0, scheme + 1) + ref;
        return detail::IsHttpUrl(absolute) ? std::optional<std::string>(absolute) : std::nullopt;
    }
    if (ref.substr(0, ref.find_first_of("/?#")).find(':') != std::string::npos)
        return std::nullopt;
    const auto authority_end = base.find_first_of("/?#", scheme + 3);
    const std::string origin = base.substr(0, authority_end);
    const auto path_end =
        base.find_first_of("?#", authority_end == std::string::npos ? base.size() : authority_end);
    const std::string base_path = authority_end != std::string::npos && base[authority_end] == '/'
                                      ? base.substr(authority_end, path_end - authority_end)
                                      : "/";
    std::string target;
    if (ref[0] == '#')
        target = base.substr(0, base.find('#')) + ref;
    else if (ref[0] == '?')
        target = origin + base_path + ref;
    else {
        const auto suffix = ref.find_first_of("?#");
        const auto path = ref.substr(0, suffix);
        std::string merged =
            path[0] == '/' ? path : base_path.substr(0, base_path.find_last_of('/') + 1) + path;
        // RFC 3986 dot-segment removal, preserving empty segments and trailing slashes.
        std::string normalized;
        while (!merged.empty()) {
            if (merged.rfind("../", 0) == 0)
                merged.erase(0, 3);
            else if (merged.rfind("./", 0) == 0)
                merged.erase(0, 2);
            else if (merged.rfind("/./", 0) == 0)
                merged.erase(0, 2);
            else if (merged == "/.")
                merged = "/";
            else if (merged.rfind("/../", 0) == 0 || merged == "/..") {
                merged.erase(0, 3);
                normalized.erase(normalized.find_last_of('/') == std::string::npos
                                     ? 0
                                     : normalized.find_last_of('/'));
                if (merged.empty())
                    merged = "/";
            } else if (merged == "." || merged == "..")
                merged.clear();
            else {
                const auto end = merged.find('/', merged[0] == '/' ? 1 : 0);
                normalized += merged.substr(0, end);
                merged.erase(0, end == std::string::npos ? merged.size() : end);
            }
        }
        target = origin + normalized + (suffix == std::string::npos ? "" : ref.substr(suffix));
    }
    return detail::IsHttpUrl(target) ? std::optional<std::string>(target) : std::nullopt;
}

// Known locations are retained even when this Xbox transport cannot browse them.
inline Unsupported LocationSupport(const std::string& url) {
    const auto lower = detail::Lower(url);
    if (lower.rfind("ftp://", 0) == 0 || lower.rfind("ftps://", 0) == 0)
        return Unsupported::Ftp;
    if (lower.rfind("gdrive:", 0) == 0)
        return Unsupported::GoogleDrive;
    if (!detail::IsHttpUrl(url))
        return Unsupported::None;
    const auto start = lower.find("://");
    if (start != std::string::npos) {
        auto host = lower.substr(start + 3, lower.find_first_of("/?#", start + 3) - start - 3);
        host.erase(0, host.rfind('@') == std::string::npos ? 0 : host.rfind('@') + 1);
        host = host.substr(0, host.find(':'));
        if (host == "1fichier.com" ||
            (host.size() > 13 && host.substr(host.size() - 13) == ".1fichier.com"))
            return Unsupported::OneFichier;
    }
    return Unsupported::None;
}
inline bool IsLocation(const std::string& url) {
    if (detail::IsHttpUrl(url))
        return true;
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= ' ' || c == 127; }))
        return false;
    const auto lower = detail::Lower(url);
    if (lower.rfind("gdrive:", 0) == 0)
        return url.size() > 7;
    const auto start = lower.rfind("ftp://", 0) == 0 ? 6 : lower.rfind("ftps://", 0) == 0 ? 7 : 0;
    return start != 0 && url.size() > static_cast<std::size_t>(start) && url[start] != '/';
}

// Strip userinfo before constructing WinRT Uri and send explicit Basic authentication.
inline std::string Authenticate(std::string url, Headers& headers) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos)
        return url;
    const auto end = url.find_first_of("/?#", scheme + 3);
    const auto at = url.rfind('@', end == std::string::npos ? url.size() : end);
    if (at == std::string::npos || at < scheme + 3)
        return url;
    const auto credentials = detail::UrlDecode(url.substr(scheme + 3, at - scheme - 3));
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (std::size_t i = 0; i < credentials.size(); i += 3) {
        unsigned bits = static_cast<unsigned char>(credentials[i]) << 16;
        if (i + 1 < credentials.size())
            bits |= static_cast<unsigned char>(credentials[i + 1]) << 8;
        if (i + 2 < credentials.size())
            bits |= static_cast<unsigned char>(credentials[i + 2]);
        encoded += alphabet[(bits >> 18) & 63];
        encoded += alphabet[(bits >> 12) & 63];
        encoded += i + 1 < credentials.size() ? alphabet[(bits >> 6) & 63] : '=';
        encoded += i + 2 < credentials.size() ? alphabet[bits & 63] : '=';
    }
    if (std::none_of(headers.begin(), headers.end(), [](const auto& header) {
            return detail::Lower(header.first) == "authorization";
        }))
        detail::AddHeader(headers, "Authorization", "Basic " + encoded);
    url.erase(scheme + 3, at - scheme - 2);
    return url;
}

// Metadata shared by classification, the shop catalog and local package matching.
struct Package {
    std::string title_id;
    std::string base_id;
    std::optional<std::uint64_t> version;
    Kind kind = Kind::Other;
    std::string name;
};

inline Package ParsePackage(const std::string& filename) {
    Package package;
    const std::string lower = detail::Lower(filename);
    std::uint64_t id = 0;
    std::size_t first_tag = filename.size();
    for (std::size_t i = 0; i < lower.size(); ++i) {
        if (lower[i] != '[')
            continue;
        const auto close = lower.find(']', i);
        if (close == std::string::npos)
            break;
        const auto tag = lower.substr(i + 1, close - i - 1);
        if (tag.size() == 16 &&
            std::all_of(tag.begin(), tag.end(), [](char c) { return detail::HexValue(c) >= 0; })) {
            package.title_id = tag;
            std::transform(
                package.title_id.begin(), package.title_id.end(), package.title_id.begin(),
                [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c; });
            id = 0;
            for (char c : tag)
                id = (id << 4) | static_cast<unsigned>(detail::HexValue(c));
            first_tag = std::min(first_tag, i);
        } else if (tag.size() >= 2 && tag.size() <= 10 && tag[0] == 'v' &&
                   std::all_of(tag.begin() + 1, tag.end(),
                               [](char c) { return c >= '0' && c <= '9'; })) {
            package.version = std::stoull(tag.substr(1));
            first_tag = std::min(first_tag, i);
        }
        i = close;
    }
    if (!package.title_id.empty()) {
        const auto tail = id & 0xFFF;
        package.kind = tail == 0 ? (package.version.value_or(0) > 0 ? Kind::Update : Kind::Game)
                       : tail == 0x800 ? Kind::Update
                                       : Kind::Dlc;
        // Add-on title IDs occupy the range one 0x1000 block above their application.
        // Older indexes also use low nonzero suffixes in the application block.
        const auto base = package.kind == Kind::Dlc && (id & 0x1000)
                              ? (id - 0x1000) & ~std::uint64_t{0xFFF}
                              : id & ~std::uint64_t{0xFFF};
        package.base_id.assign(16, '0');
        for (int i = 0; i < 16; ++i)
            package.base_id[15 - i] = "0123456789ABCDEF"[(base >> (i * 4)) & 15];
    } else if (lower.find("dlc") != std::string::npos) {
        package.kind = Kind::Dlc;
    } else if (package.version) {
        package.kind = *package.version > 0 ? Kind::Update : Kind::Game;
    }
    package.name = detail::Trim(filename.substr(0, first_tag));
    if (first_tag == filename.size()) {
        const auto dot = package.name.find_last_of('.');
        if (dot != std::string::npos)
            package.name.resize(dot);
    }
    if (package.name.empty())
        package.name = filename;
    return package;
}

inline Kind Classify(const std::string& name) {
    return ParsePackage(name).kind;
}

inline Index ParseIndex(std::string_view body, const std::string& base) {
    Index index;
    if (body.substr(0, 3) == "\xEF\xBB\xBF")
        body.remove_prefix(3);
    if (body.substr(0, 7) == "TINFOIL") {
        index.unsupported = Unsupported::Encrypted;
        return index;
    }
    detail::Value root;
    if (!detail::JsonReader(body).Read(root) || root.type != detail::Value::Type::Object) {
        root = {};
        // Autoindex links, independent of Apache/nginx/IIS table markup. Ignore parents,
        // sorting queries, external links and non-package files. Never execute page content.
        const std::string html(body);
        const auto lower = detail::Lower(html);
        detail::Value files, directories;
        files.type = directories.type = detail::Value::Type::Array;
        std::size_t cursor = 0;
        while ((cursor = lower.find("<a", cursor)) != std::string::npos &&
               files.array.size() + directories.array.size() < kMaxEntries) {
            const auto end = lower.find('>', cursor);
            if (end == std::string::npos)
                break;
            const auto href = lower.find("href", cursor + 2);
            cursor = end + 1;
            if (href == std::string::npos || href >= end)
                continue;
            auto start = lower.find('=', href + 4);
            if (start == std::string::npos || start >= end)
                continue;
            do {
                ++start;
            } while (start < end && std::isspace(static_cast<unsigned char>(html[start])));
            if (start >= end)
                continue;
            const char quote = html[start];
            const bool quoted = quote == '\'' || quote == '"';
            if (quoted)
                ++start;
            auto stop = start;
            while (stop < end && (quoted ? html[stop] != quote
                                         : !std::isspace(static_cast<unsigned char>(html[stop]))))
                ++stop;
            std::string link = html.substr(start, stop - start);
            for (const auto& entity :
                 {std::make_pair("&amp;", "&"), std::make_pair("&#38;", "&"),
                  std::make_pair("&quot;", "\""), std::make_pair("&#39;", "'")}) {
                std::size_t at = 0;
                while ((at = link.find(entity.first, at)) != std::string::npos) {
                    link.replace(at, std::char_traits<char>::length(entity.first), entity.second);
                    ++at;
                }
            }
            const auto absolute = Resolve(base, link);
            if (!absolute || link.empty() || link[0] == '?' || link[0] == '#')
                continue;
            const auto path =
                detail::Lower(detail::UrlDecode(link.substr(0, link.find_first_of("?#"))));
            const bool directory =
                !path.empty() && path.back() == '/' && path != "../" && path != "./";
            const auto extension = path.size() >= 4 ? path.substr(path.size() - 4) : "";
            if (extension != ".nsp" && extension != ".nsz" && extension != ".xci" &&
                extension != ".xcz" && !directory)
                continue;
            if (directory && (absolute->rfind(base.substr(0, base.find_first_of("?#")), 0) != 0 ||
                              *absolute == base))
                continue;
            detail::Value value;
            value.type = detail::Value::Type::String;
            value.text = *absolute;
            (directory ? directories : files).array.push_back(std::move(value));
        }
        if (files.array.empty() && directories.array.empty() &&
            lower.find("index of ") == std::string::npos &&
            lower.find("directory listing") == std::string::npos)
            return index;
        root.type = detail::Value::Type::Object;
        root.object.emplace_back("files", std::move(files));
        root.object.emplace_back("directories", std::move(directories));
    }
    index.status = Status::Ok;
    index.success = root.At("success").String();
    index.error = root.At("error").String();
    if (!index.error.empty()) {
        index.status = Status::SourceError;
        return index;
    }
    const auto& locations = root.At("locations");
    const auto add_location = [&](const std::string& value) {
        if (index.locations.size() >= kMaxIndexes)
            return;
        if (IsLocation(value))
            index.locations.push_back(value);
        else if (const auto url = Resolve(base, value))
            index.locations.push_back(*url);
    };
    if (locations.type == detail::Value::Type::String)
        add_location(locations.text);
    for (const auto& item : locations.array)
        add_location(item.type == detail::Value::Type::String ? item.text
                                                              : item.At("url").String());
    if (locations.type == detail::Value::Type::Object)
        for (const auto& item : locations.object)
            add_location(item.second.String());
    if (const auto referrer = root.At("referrer").String(); !referrer.empty())
        detail::AddHeader(index.headers, "Referer", referrer);
    detail::ReadHeaders(root.At("headers"), index.headers);
    for (const auto& item : root.At("files").array) {
        if (index.files.size() >= kMaxEntries)
            break;
        const auto reference =
            item.type == detail::Value::Type::String ? item.text : item.At("url").String();
        const auto absolute = Resolve(base, reference);
        if (!absolute)
            continue;
        File file;
        const auto hash = absolute->find('#');
        file.url = absolute->substr(0, hash);
        if (hash != std::string::npos)
            file.name = detail::UrlDecode(absolute->substr(hash + 1));
        if (file.name.empty()) {
            const auto path = file.url.substr(0, file.url.find('?'));
            const auto last = path.find_last_of('/');
            if (last != std::string::npos && last > path.find("://") + 2)
                file.name = detail::UrlDecode(path.substr(last + 1));
        }
        if (file.name.empty()) {
            const auto start = file.url.find("://") + 3;
            file.name = file.url.substr(start, file.url.find_first_of("/?#", start) - start);
            const auto at = file.name.rfind('@');
            if (at != std::string::npos)
                file.name.erase(0, at + 1);
        }
        file.kind = Classify(file.name);
        file.size = detail::Size(item.At("size"));
        index.files.push_back(std::move(file)); // keep duplicate files, as the screen did
    }
    for (const auto& item : root.At("directories").array) {
        const auto reference =
            item.type == detail::Value::Type::String ? item.text : item.At("url").String();
        if (auto absolute = Resolve(base, reference)) {
            absolute->erase(absolute->find('#') == std::string::npos ? absolute->size()
                                                                     : absolute->find('#'));
            index.directories.push_back(std::move(*absolute));
        }
    }
    return index;
}

struct Request {
    std::string url;
    std::size_t depth = 0;
    Headers headers;
};
// A bounded breadth-first queue shared by host tests and the UI. It performs no I/O.
class Traversal {
public:
    explicit Traversal(std::string url) {
        url.erase(url.find('#') == std::string::npos ? url.size() : url.find('#'));
        visited_.insert(url);
        Headers headers;
        Authenticate(url, headers);
        requests_.push_back({std::move(url), 0, std::move(headers)});
    }
    void AddRoot(std::string url) {
        url = url.substr(0, url.find('#'));
        if (requests_.size() < kMaxIndexes && visited_.insert(url).second) {
            Headers headers;
            Authenticate(url, headers);
            requests_.push_back({std::move(url), 0, std::move(headers)});
        }
    }
    std::optional<Request> Next() {
        if (next_ == requests_.size())
            return std::nullopt;
        return requests_[next_++];
    }
    bool HasNext() const {
        return next_ < requests_.size();
    }
    void Follow(const Request& parent, const Index& index) {
        if (index.status != Status::Ok || parent.depth >= kMaxDepth)
            return;
        Headers headers = parent.headers;
        for (const auto& header : index.headers) {
            headers.erase(std::remove_if(headers.begin(), headers.end(),
                                         [&](const auto& old) {
                                             return detail::Lower(old.first) ==
                                                    detail::Lower(header.first);
                                         }),
                          headers.end());
            headers.push_back(header);
        }
        for (const auto& directory : index.directories) {
            if (requests_.size() >= kMaxIndexes)
                break;
            if (visited_.insert(directory).second)
                requests_.push_back({directory, parent.depth + 1, headers});
        }
    }

private:
    std::set<std::string> visited_;
    std::vector<Request> requests_;
    std::size_t next_ = 0;
};
} // namespace EdenXbox::Tinfoil
