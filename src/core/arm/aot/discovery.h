// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace Core::Aot {
using Address = std::uint64_t;
struct Range {
    Address address;
    std::vector<std::uint8_t> bytes;
    bool executable;
};
struct Module {
    std::vector<Range> ranges;
    std::vector<Address> roots;
    std::vector<std::uint8_t> build_id;
};
struct Limits {
    std::size_t instructions = 4 * 1024 * 1024;
    std::size_t blocks = 262144;
};
struct Discovery {
    std::set<Address> blocks;
    std::size_t instructions{};
    std::size_t unresolved_indirect{};
    bool truncated{};
};
inline bool Contains(const Range& range, Address pc, std::size_t size) {
    return pc >= range.address && pc - range.address <= range.bytes.size() &&
           size <= range.bytes.size() - (pc - range.address);
}
inline std::optional<std::uint64_t> Read(std::span<const std::uint8_t> bytes, std::size_t offset,
                                         std::size_t size) {
    if (size > 8 || offset > bytes.size() || size > bytes.size() - offset) {
        return std::nullopt;
    }
    std::uint64_t value{};
    for (std::size_t i = 0; i < size; ++i) {
        value |= std::uint64_t{bytes[offset + i]} << (8 * i);
    }
    return value;
}
inline std::optional<std::uint32_t> Instruction(const Module& module, Address pc) {
    if (pc % 4) {
        return std::nullopt;
    }
    for (const auto& range : module.ranges) {
        if (range.executable && Contains(range, pc, 4)) {
            return static_cast<std::uint32_t>(*Read(range.bytes, pc - range.address, 4));
        }
    }
    return std::nullopt;
}
inline std::optional<Address> Relative(Address pc, std::uint32_t immediate, unsigned bits) {
    const std::int64_t displacement = (static_cast<std::int64_t>(immediate) -
                                       ((immediate & (1U << (bits - 1))) ? (1LL << bits) : 0)) *
                                      4;
    if (displacement < 0) {
        const auto magnitude = static_cast<Address>(-displacement);
        return pc >= magnitude ? std::optional<Address>{pc - magnitude} : std::nullopt;
    }
    const auto magnitude = static_cast<Address>(displacement);
    return pc <= std::numeric_limits<Address>::max() - magnitude
               ? std::optional<Address>{pc + magnitude}
               : std::nullopt;
}
// Conservative candidates are never executed. Pointer scanning covers absolute relocated
// init arrays, vtables and jump tables; relative/computed tables remain runtime fallbacks.
inline Discovery Discover(const Module& module, Limits limits = {}) {
    Discovery result;
    std::deque<Address> pending;
    auto seed = [&](Address pc) {
        if (!Instruction(module, pc) || result.blocks.contains(pc)) {
            return;
        }
        if (result.blocks.size() >= limits.blocks) {
            result.truncated = true;
            return;
        }
        result.blocks.insert(pc);
        pending.push_back(pc);
    };
    for (auto pc : module.roots) {
        seed(pc);
    }
    std::size_t scanned{};
    for (const auto& range : module.ranges) {
        const auto stride = range.executable ? 4U : 8U;
        for (std::size_t offset = 0; offset + stride <= range.bytes.size(); offset += stride) {
            if (scanned++ >= limits.instructions) {
                result.truncated = true;
                break;
            }
            const auto word = *Read(range.bytes, offset, stride);
            if (!range.executable) {
                seed(word);
            } else if ((word & 0xffc07fffU) == 0xa9807bfdU ||        // STP x29,x30,[sp,#imm]!
                       word == 0xd503245fU || word == 0xd503233fU) { // BTI c / PACIASP
                seed(range.address + offset);
            }
        }
    }
    std::set<Address> visited;
    while (!pending.empty()) {
        auto pc = pending.front();
        pending.pop_front();
        while (auto instruction = Instruction(module, pc)) {
            if (!visited.insert(pc).second) {
                break;
            }
            if (result.instructions >= limits.instructions) {
                result.truncated = true;
                return result;
            }
            ++result.instructions;
            const auto word = *instruction;
            const auto next = pc <= std::numeric_limits<Address>::max() - 4
                                  ? std::optional<Address>{pc + 4}
                                  : std::nullopt;
            std::optional<Address> target;
            bool terminal = false, fallthrough = false;
            if ((word & 0x7c000000U) == 0x14000000U) { // B / BL
                target = Relative(pc, word & 0x03ffffffU, 26);
                terminal = true;
                fallthrough = (word & 0x80000000U) != 0;
            } else if ((word & 0xff000010U) == 0x54000000U ||
                       (word & 0x7e000000U) == 0x34000000U) { // B.cond / CBZ / CBNZ
                target = Relative(pc, (word >> 5) & 0x7ffffU, 19);
                terminal = fallthrough = true;
            } else if ((word & 0x7e000000U) == 0x36000000U) { // TBZ / TBNZ
                target = Relative(pc, (word >> 5) & 0x3fffU, 14);
                terminal = fallthrough = true;
            } else if ((word & 0xfffffc1fU) == 0xd61f0000U ||
                       (word & 0xfffffc1fU) == 0xd63f0000U) { // BR / BLR (including PLT)
                ++result.unresolved_indirect;
                terminal = true;
                fallthrough = (word & 0xfffffc1fU) == 0xd63f0000U;
            } else if ((word & 0xfffffc1fU) == 0xd65f0000U || (word & 0xffe0001fU) == 0xd4000001U ||
                       (word & 0xffe0001fU) == 0xd4200000U || word == 0) { // RET/SVC/BRK/UDF
                terminal = true;
                fallthrough = (word & 0xffe0001fU) == 0xd4000001U;
            }
            if (target) {
                seed(*target);
            }
            if (terminal) {
                if (fallthrough && next) {
                    seed(*next);
                }
                break;
            }
            if (!next) {
                break;
            }
            pc = *next;
        }
    }
    return result;
}
} // namespace Core::Aot
