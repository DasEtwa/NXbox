// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Shader::Backend::HLSL {
// These passes run on every pipeline the console builds. std::regex took about 15 ms per shader on
// the corpus for the helper strip alone; the hand scanners below give the same matches.
inline constexpr bool IsHlslWord(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
inline constexpr bool IsHlslSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Matches `^(v\d+)\s*=\s*(?!=)([^;{}]+);$` with std::regex_match's backtracking: a blank before
// `=` or before the final `;` is handed to the expression, as the regex did.
inline bool ParseAssignment(std::string_view line, std::string_view& name,
                            std::string_view& expression) {
    const size_t n = line.size();
    if (n == 0 || line[0] != 'v' || line[n - 1] != ';')
        return false;
    size_t digits = 1;
    while (digits < n && line[digits] >= '0' && line[digits] <= '9')
        ++digits;
    if (digits == 1)
        return false;
    size_t equal = digits;
    while (equal < n && IsHlslSpace(line[equal]))
        ++equal;
    if (equal >= n || line[equal] != '=')
        return false;
    const size_t after = equal + 1;
    size_t start = after;
    while (start < n && IsHlslSpace(line[start]))
        ++start;
    if ((start < n && line[start] == '=') || start >= n - 1) {
        if (start == after)
            return false;
        --start;
    }
    const auto body = line.substr(start, n - 1 - start);
    if (body.find_first_of(";{}") != body.npos)
        return false;
    name = line.substr(0, digits);
    expression = body;
    return true;
}

// The name in a `[attribute]* [precise] type name(` function head, or empty for anything else.
inline std::string_view FunctionName(std::string_view head) {
    size_t i = 0;
    const auto skip_space = [&] {
        while (i < head.size() && IsHlslSpace(head[i]))
            ++i;
    };
    skip_space();
    while (i < head.size() && head[i] == '[') {
        const size_t close = head.find(']', i + 1);
        if (close == head.npos)
            return {};
        i = close + 1;
        skip_space();
    }
    const auto parse = [&](size_t at) -> std::string_view {
        size_t type_end = at;
        while (type_end < head.size() &&
               (IsHlslWord(head[type_end]) || head[type_end] == '<' || head[type_end] == '>'))
            ++type_end;
        if (type_end == at || type_end == head.size() || !IsHlslSpace(head[type_end]))
            return {};
        size_t name = type_end;
        while (name < head.size() && IsHlslSpace(head[name]))
            ++name;
        size_t name_end = name;
        while (name_end < head.size() && IsHlslWord(head[name_end]))
            ++name_end;
        size_t paren = name_end;
        while (paren < head.size() && IsHlslSpace(head[paren]))
            ++paren;
        if (name_end == name || paren == head.size() || head[paren] != '(')
            return {};
        return head.substr(name, name_end - name);
    };
    constexpr std::string_view precise = "precise";
    if (head.substr(i, precise.size()) == precise && i + precise.size() < head.size() &&
        IsHlslSpace(head[i + precise.size()])) {
        size_t after = i + precise.size();
        while (after < head.size() && IsHlslSpace(head[after]))
            ++after;
        if (const auto name = parse(after); !name.empty())
            return name;
    }
    return parse(i);
}

// Calls `visit(name)` for every `identifier(` in `text`, identifiers starting with a letter or `_`.
template <typename Visit>
void ForEachCall(std::string_view text, Visit&& visit) {
    for (size_t i = 0; i < text.size();) {
        if (!IsHlslWord(text[i]) || (i > 0 && IsHlslWord(text[i - 1]))) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < text.size() && IsHlslWord(text[end]))
            ++end;
        size_t paren = end;
        while (paren < text.size() && IsHlslSpace(text[paren]))
            ++paren;
        if (!(text[i] >= '0' && text[i] <= '9') && paren < text.size() && text[paren] == '(') {
            visit(text.substr(i, end - i));
            i = paren + 1;
        } else {
            i = end;
        }
    }
}

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

inline constexpr bool IsHlslTypeName(std::string_view word) {
    constexpr std::array<std::string_view, 21> scalars{
        "bool",     "int",       "uint",       "dword",     "half",     "float",    "double",
        "min16float", "min10float", "min16int", "min12int", "min16uint", "int16_t",  "uint16_t",
        "int32_t",  "uint32_t",  "int64_t",    "uint64_t",  "float16_t", "float32_t", "float64_t"};
    const auto dimension = [](char c) { return c >= '1' && c <= '4'; };
    return std::ranges::any_of(scalars, [&](std::string_view scalar) {
        if (!word.starts_with(scalar))
            return false;
        const auto rest = word.substr(scalar.size());
        return rest.empty() || (rest.size() == 1 && dimension(rest[0])) ||
               (rest.size() == 3 && dimension(rest[0]) && rest[1] == 'x' && dimension(rest[2]));
    });
}

// HLSL parses like C++: in "(float(float(v1)))" the parentheses can hold the function type
// float(float), and DXC reads the group as a cast when a unary operator follows, so
// "(float(float(v1)))+v2" stops compiling. glslang accepts it, which is why the SPIR-V check
// missed it. True for text that starts with a cast whose argument starts with another type name.
inline bool StartsWithNestedCast(std::string_view text) {
    size_t i = 0;
    const auto skip = [&](bool parentheses) {
        while (i < text.size() && (IsHlslSpace(text[i]) || (parentheses && text[i] == '(')))
            ++i;
    };
    const auto word = [&] {
        const size_t start = i;
        while (i < text.size() && IsHlslWord(text[i]))
            ++i;
        return text.substr(start, i - start);
    };
    skip(false);
    if (!IsHlslTypeName(word()))
        return false;
    skip(false);
    if (i == text.size() || text[i] != '(')
        return false;
    ++i;
    skip(true);
    return IsHlslTypeName(word());
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
    InlineBlockResult result;
    for (size_t index = 0; index < lines.size(); ++index) {
        std::string_view assigned;
        std::string_view assigned_expression;
        if (!ParseAssignment(lines[index], assigned, assigned_expression))
            continue;
        const std::string name{assigned};
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
        const std::string expression{assigned_expression};
        // The dynamic cbuffer helper contains a switch. Keep the evaluation order
        // of separate lookups instead of moving those control-flow regions into operands.
        if (expression.size() > max_expression ||
            expression.find("nx_cbuf_load(") != expression.npos)
            continue;
        // Preserve the conversion performed by the original assignment, including
        // signed-to-unsigned conversion and scalar/vector truncation.
        const std::string inner = type->second.exact_type
                                      ? expression
                                      : type->second.type + "(" + expression + ")";
        const std::string replacement = "(" + inner + ")";
        const auto tokens = occurrences(consumer);
        const auto token = std::find_if(tokens.begin(), tokens.end(),
                                        [&](const auto& item) { return item.first == name; });
        if (token == tokens.end() || consumer.size() - name.size() + replacement.size() > max_line)
            continue;
        // DXC only takes the group as a cast when what follows can start an operand.
        const auto follower = consumer.find_first_not_of(" \t", token->second + name.size());
        if (StartsWithNestedCast(inner) && follower != consumer.npos &&
            (IsHlslWord(consumer[follower]) ||
             std::string_view{"+-*&!~(.["}.find(consumer[follower]) != std::string_view::npos))
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
        std::string_view name;
    };
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
                if (const auto name = FunctionName(source.substr(begin, brace - begin));
                    !name.empty())
                    functions.push_back({gap, begin, end, name});
                const size_t next = source.find_first_not_of(" \t\r\n", end);
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
    std::unordered_map<std::string_view, std::vector<size_t>> overloads;
    for (size_t index = 0; index < functions.size(); ++index)
        overloads[functions[index].name].push_back(index);
    if (!overloads.contains("main"))
        return std::string{source};
    // Each reachable function is scanned once, all overloads of a reachable name included.
    std::unordered_set<std::string_view> reachable{"main"};
    std::vector<std::string_view> pending{"main"};
    while (!pending.empty()) {
        const auto name = pending.back();
        pending.pop_back();
        const auto found = overloads.find(name);
        if (found == overloads.end())
            continue;
        for (const size_t index : found->second) {
            const auto& fn = functions[index];
            ForEachCall(source.substr(fn.begin, fn.end - fn.begin), [&](std::string_view callee) {
                if (reachable.insert(callee).second)
                    pending.push_back(callee);
            });
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
