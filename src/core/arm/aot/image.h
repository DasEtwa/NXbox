// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/arm/aot/discovery.h"

namespace Core::Aot {
// Host-side input adapter. Production loaders should supply their final decompressed,
// patched, relocated ranges instead. Never scan encrypted/compressed input as instructions.
inline std::optional<Module> ParseImage(std::span<const std::uint8_t> file, Address base = 0) {
    constexpr std::size_t image_limit = 64 * 1024 * 1024;
    Module module;
    std::size_t copied{};
    auto field = [&](std::size_t offset, std::size_t size) { return Read(file, offset, size); };
    auto append = [&](Address address, std::uint64_t offset, std::uint64_t size, bool executable) {
        if (offset > file.size() || size > file.size() - offset || size > image_limit - copied ||
            address > std::numeric_limits<Address>::max() - base ||
            size > std::numeric_limits<Address>::max() - (base + address)) {
            return false;
        }
        module.ranges.push_back(
            {base + address, {file.begin() + offset, file.begin() + offset + size}, executable});
        copied += size;
        return true;
    };
    if (field(0, 4) == 0x304f534eU) { // NSO0, uncompressed segments only
        if (file.size() < 0x100 || (*field(0xc, 4) & 0x87U) != 0) {
            return std::nullopt;
        }
        for (unsigned i = 0; i < 3; ++i) {
            const auto at = 0x10 + i * 0x10;
            if (!append(*field(at + 4, 4), *field(at, 4), *field(at + 8, 4), i == 0)) {
                return std::nullopt;
            }
        }
        module.build_id.assign(file.begin() + 0x40, file.begin() + 0x60);
        module.roots.push_back(module.ranges[0].address);
        // NSO dynsym extent is relative to rodata, with ELF64 symbol entries.
        const auto offset = *field(0x88, 4), size = *field(0x8c, 4);
        const auto& ro = module.ranges[1];
        if (size % 24 || offset > ro.bytes.size() || size > ro.bytes.size() - offset) {
            return std::nullopt;
        }
        for (std::size_t at = offset; at < offset + size; at += 24) {
            const auto info = *Read(ro.bytes, at + 4, 1);
            const auto section = *Read(ro.bytes, at + 6, 2);
            const auto value = *Read(ro.bytes, at + 8, 8);
            if ((info & 15) == 2 && section &&
                value <= std::numeric_limits<Address>::max() - base) {
                module.roots.push_back(base + value);
            }
        }
    } else if (field(0x10, 4) == 0x304f524eU) { // NRO0
        if (file.size() < 0x80 || *field(0x18, 4) > file.size()) {
            return std::nullopt;
        }
        for (unsigned i = 0; i < 3; ++i) {
            const auto at = 0x20 + i * 8;
            if (!append(*field(at, 4), *field(at, 4), *field(at + 4, 4), i == 0)) {
                return std::nullopt;
            }
        }
        module.build_id.assign(file.begin() + 0x40, file.begin() + 0x60);
        module.roots.push_back(module.ranges[0].address);
    } else if (field(0, 4) == 0x464c457fU) { // little-endian ELF64 AArch64
        if (file.size() < 64 || field(4, 1) != 2 || field(5, 1) != 1 || field(18, 2) != 183 ||
            field(54, 2) != 56 || (*field(24, 8) > std::numeric_limits<Address>::max() - base)) {
            return std::nullopt;
        }
        module.roots.push_back(base + *field(24, 8));
        const auto phoff = *field(32, 8), count = *field(56, 2);
        if (phoff > file.size() || count > (file.size() - phoff) / 56) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto at = phoff + i * 56;
            if (field(at, 4) == 1 && !append(*field(at + 16, 8), *field(at + 8, 8),
                                             *field(at + 32, 8), (*field(at + 4, 4) & 1) != 0)) {
                return std::nullopt;
            }
        }
        const auto shoff = *field(40, 8), sections = *field(60, 2);
        if (sections &&
            (field(58, 2) != 64 || shoff > file.size() || sections > (file.size() - shoff) / 64)) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < sections; ++i) {
            const auto at = shoff + i * 64;
            const auto type = *field(at + 4, 4), offset = *field(at + 24, 8),
                       size = *field(at + 32, 8);
            if (type != 2 && type != 11 && type != 14 && type != 4) {
                continue;
            }
            const auto stride = type == 14 ? 8U : 24U;
            if (offset > file.size() || size > file.size() - offset || size % stride ||
                (type != 14 && field(at + 56, 8) != 24)) {
                return std::nullopt;
            }
            for (std::size_t pos = offset; pos < offset + size; pos += stride) {
                if (type == 4) {
                    // R_AARCH64_RELATIVE: relocated init/vtable entries point to base+addend.
                    if ((*field(pos + 8, 8) & 0xffffffffU) == 1027) {
                        const auto addend = *field(pos + 16, 8);
                        if (addend <= std::numeric_limits<Address>::max() - base) {
                            module.roots.push_back(base + addend);
                        }
                    }
                    continue;
                }
                if (type != 14 && ((*field(pos + 4, 1) & 15) != 2 || !*field(pos + 6, 2))) {
                    continue;
                }
                const auto pc = *field(pos + (type == 14 ? 0 : 8), 8);
                if (pc <= std::numeric_limits<Address>::max() - base) {
                    module.roots.push_back(base + pc);
                }
            }
        }
    } else {
        return std::nullopt;
    }
    if (module.ranges.empty()) {
        return std::nullopt;
    }
    return module;
}
} // namespace Core::Aot
