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
#include <unordered_map>
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

struct InlineCandidate {
    std::string name;
    std::string type;
    bool exact_type{};
};
struct InlineBlockResult {
    std::string source;
    std::vector<std::string> eliminated;
};

// Candidates are SSA results with a single IR consumer in this block. The source
// checks also reject multiple HLSL writes, scopes, out parameters and lazy uses.
inline InlineBlockResult InlineSingleUseBlock(std::string_view source,
                                              const std::vector<InlineCandidate>& candidates,
                                              size_t max_expression = 600, size_t max_line = 3000) {
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < source.size();) {
        const auto end = source.find('\n', pos);
        lines.emplace_back(
            source.substr(pos, end == source.npos ? source.size() - pos : end - pos));
        pos = end == source.npos ? source.size() : end + 1;
    }
    const auto occurrences = [](std::string_view line) {
        std::vector<std::pair<std::string, size_t>> result;
        for (size_t pos = 0; pos < line.size();) {
            const auto start = pos;
            const auto is_identifier = [](char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_';
            };
            if (!is_identifier(line[pos])) {
                ++pos;
                continue;
            }
            while (pos < line.size() && is_identifier(line[pos]))
                ++pos;
            const auto token = line.substr(start, pos - start);
            if (token.size() > 1 && token[0] == 'v' &&
                token.find_first_not_of("0123456789", 1) == token.npos)
                result.emplace_back(token, start);
        }
        return result;
    };
    std::unordered_map<std::string, std::vector<size_t>> sites;
    std::unordered_map<std::string, InlineCandidate> types;
    for (const auto& candidate : candidates)
        types.emplace(candidate.name, candidate);
    std::vector<size_t> segments(lines.size());
    size_t segment = 0;
    for (size_t index = 0; index < lines.size(); ++index) {
        const bool scope = lines[index].find_first_of("{}") != std::string::npos;
        if (scope)
            ++segment;
        segments[index] = segment;
        if (scope)
            ++segment;
        for (const auto& [name, offset] : occurrences(lines[index])) {
            (void)offset;
            sites[name].push_back(index);
        }
    }
    static const std::regex assignment{R"(^(v\d+)\s*=\s*(?!=)([^;{}]+);$)"};
    InlineBlockResult result;
    for (size_t index = 0; index < lines.size(); ++index) {
        std::smatch match;
        if (!std::regex_match(lines[index], match, assignment))
            continue;
        const std::string name = match[1];
        const auto type = types.find(name);
        if (type == types.end() || type->second.type.starts_with("precise "))
            continue;
        const auto found = sites.find(name);
        if (found == sites.end() || found->second.size() != 2 || found->second[0] != index)
            continue;
        const size_t use = found->second[1];
        if (use <= index || segments[index] != segments[use])
            continue;
        const auto& consumer = lines[use];
        if (consumer.find('?') != consumer.npos || consumer.find("&&") != consumer.npos ||
            consumer.find("||") != consumer.npos || consumer.find("Interlocked") != consumer.npos ||
            consumer.find("GetDimensions") != consumer.npos ||
            consumer.find("uaddCarry") != consumer.npos || consumer.find("sincos") != consumer.npos)
            continue;
        const std::string expression = match[2];
        // The dynamic cbuffer helper contains a switch. Keep the evaluation order
        // of separate lookups instead of moving those control-flow regions into operands.
        if (expression.size() > max_expression ||
            expression.find("nx_cbuf_load(") != expression.npos)
            continue;
        // Preserve the conversion performed by the original assignment, including
        // signed-to-unsigned conversion and scalar/vector truncation.
        const std::string replacement = type->second.exact_type
                                            ? "(" + expression + ")"
                                            : "(" + type->second.type + "(" + expression + "))";
        const auto tokens = occurrences(consumer);
        const auto token = std::find_if(tokens.begin(), tokens.end(),
                                        [&](const auto& item) { return item.first == name; });
        if (token == tokens.end() || consumer.size() - name.size() + replacement.size() > max_line)
            continue;
        std::string rewritten = consumer;
        rewritten.replace(token->second, name.size(), replacement);
        // Redirect the expression's dependencies before deleting its assignment.
        for (const auto& [operand, offset] : occurrences(expression)) {
            (void)offset;
            auto& references = sites[operand];
            const auto reference = std::find(references.begin(), references.end(), index);
            if (reference != references.end())
                *reference = use;
            std::sort(references.begin(), references.end());
        }
        lines[use] = std::move(rewritten);
        lines[index].clear();
        result.eliminated.push_back(name);
    }
    for (const auto& line : lines)
        if (!line.empty())
            result.source += line + '\n';
    return result;
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
