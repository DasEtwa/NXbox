// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cctype>
#include <string_view>

namespace D3D12 {
// A source-level warning signal, not a proof that an index survives DXC optimization.
inline size_t CountNonliteralIndices(std::string_view source, std::string_view array) {
    size_t count{}, position{};
    while ((position = source.find(array, position)) != std::string_view::npos) {
        position += array.size();
        const auto end = source.find(']', position);
        if (end == std::string_view::npos)
            break;
        const auto index = source.substr(position, end - position);
        bool literal = !index.empty(), has_digit{};
        for (const unsigned char character : index) {
            has_digit |= std::isdigit(character) != 0;
            literal &= std::isdigit(character) != 0 || std::isspace(character) != 0 ||
                       character == 'u' || character == 'U';
        }
        count += !(literal && has_digit);
        position = end + 1;
    }
    return count;
}
inline size_t CountLlvmInstructions(std::string_view assembly) {
    size_t count{};
    while (!assembly.empty()) {
        const auto end = assembly.find('\n');
        auto line = assembly.substr(0, end);
        if (line.starts_with("  ")) {
            line.remove_prefix(2);
            if ((line.starts_with('%') && line.find(" = ") != std::string_view::npos) ||
                line.starts_with("store ") || line.starts_with("br ") || line.starts_with("ret ") ||
                line.starts_with("call ") || line.starts_with("switch ") ||
                line.starts_with("unreachable") || line.starts_with("fence ") ||
                line.starts_with("indirectbr ") || line.starts_with("resume "))
                ++count;
        }
        if (end == std::string_view::npos)
            break;
        assembly.remove_prefix(end + 1);
    }
    return count;
}
} // namespace D3D12
