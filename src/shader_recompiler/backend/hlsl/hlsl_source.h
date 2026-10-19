// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Shader::Backend::HLSL {
// Decimal spelling is allowed only when parsing it reproduces the original float.
// Preserve exceptional values, signed zero and subnormals as explicit bit patterns.
inline std::string FormatFloatImmediate(std::uint32_t bits) {
    const auto bit_literal = [bits] {
        std::array<char, 8> digits{};
        const auto converted =
            std::to_chars(digits.data(), digits.data() + digits.size(), bits, 16);
        return "asfloat(0x" +
               std::string(8 - static_cast<size_t>(converted.ptr - digits.data()), '0') +
               std::string(digits.data(), converted.ptr) + "u)";
    };
    const auto magnitude = bits & 0x7fffffffu;
    if (magnitude >= 0x7f800000u || bits == 0x80000000u ||
        (magnitude != 0 && magnitude < 0x00800000u))
        return bit_literal();
    std::array<char, 64> buffer{};
    const auto converted =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::bit_cast<float>(bits));
    if (converted.ec != std::errc{})
        return bit_literal();
    float roundtrip{};
    const auto parsed = std::from_chars(buffer.data(), converted.ptr, roundtrip);
    if (parsed.ec != std::errc{} || parsed.ptr != converted.ptr ||
        std::bit_cast<std::uint32_t>(roundtrip) != bits)
        return bit_literal();
    std::string literal(buffer.data(), converted.ptr);
    if (literal.find_first_of(".eE") == literal.npos)
        literal += ".0";
    literal += 'f';
    return bits >> 31 ? "(" + literal + ")" : literal;
}

// A cbuffer is an array of uint4s. Preserve the helper for unaligned, dynamic,
// undeclared and out-of-range accesses, including unsigned offset wraparound.
inline constexpr bool IsDirectCbufferAddress(std::optional<std::uint32_t> binding,
                                             std::optional<std::uint32_t> offset, bool declared,
                                             std::uint32_t vectors = 4096) {
    return binding && offset && declared && (*offset & 3u) == 0 && (*offset / 16u) < vectors;
}

// This scans the emitter's regular top-level declarations, not arbitrary user HLSL.
// Keep all overloads of a reachable name: resolving HLSL overloads belongs to the compiler.
inline std::string StripUncalledHelpers(std::string_view source) {
    struct Function {
        size_t gap;
        size_t begin;
        size_t end;
        std::string name;
    };
    static const std::regex signature{
        R"(^\s*(?:\[[^\]]*\]\s*)*(?:precise\s+)?[\w<>]+\s+(\w+)\s*\()"};
    static const std::regex call{R"(\b([A-Za-z_]\w*)\s*\()"};
    std::vector<Function> functions;
    size_t gap = 0;
    size_t begin = 0;
    size_t depth = 0;
    size_t brace = source.npos;
    for (size_t pos = 0; pos < source.size(); ++pos) {
        const char ch = source[pos];
        if (ch == '{') {
            if (depth++ == 0)
                brace = pos;
        } else if ((ch == '}' && depth && --depth == 0) || (ch == ';' && depth == 0)) {
            size_t end = pos + 1;
            if (brace != source.npos) {
                const std::string head{source.substr(begin, brace - begin)};
                std::smatch match;
                if (std::regex_search(head, match, signature))
                    functions.push_back({gap, begin, end, match[1].str()});
                size_t next = source.find_first_not_of(" \t\r\n", end);
                if (next != source.npos && source[next] == ';')
                    end = next + 1;
            }
            gap = end;
            begin = source.find_first_not_of(" \t\r\n", end);
            if (begin == source.npos)
                begin = source.size();
            pos = begin ? begin - 1 : 0;
            brace = source.npos;
        }
    }
    std::unordered_set<std::string> reachable{"main"};
    if (std::none_of(functions.begin(), functions.end(),
                     [](const auto& fn) { return fn.name == "main"; }))
        return std::string{source};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& fn : functions) {
            if (!reachable.contains(fn.name))
                continue;
            const std::string body{source.substr(fn.begin, fn.end - fn.begin)};
            for (std::sregex_iterator it{body.begin(), body.end(), call}, end; it != end; ++it)
                changed |= reachable.emplace((*it)[1].str()).second;
        }
    }
    std::string result;
    size_t copied = 0;
    for (const auto& fn : functions) {
        if (reachable.contains(fn.name))
            continue;
        result.append(source.substr(copied, fn.gap - copied));
        copied = fn.end;
    }
    result.append(source.substr(copied));
    return result;
}
} // namespace Shader::Backend::HLSL
