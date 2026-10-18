// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace D3D12 {
// Constructed only when dumping is enabled. Preserve distinct PSO specializations of a
// guest stage instead of overwriting them; worker threads share the same deduplication.
class HlslDump {
public:
    void Write(const std::filesystem::path& directory, std::uint64_t guest_hash, int stage,
               std::string_view source) {
        std::scoped_lock lock{mutex};
        std::uint64_t source_hash = 14695981039346656037ULL;
        for (const unsigned char byte : source)
            source_hash = (source_hash ^ byte) * 1099511628211ULL;
        auto& variants = written[{guest_hash, stage}];
        if (variants.contains(source_hash))
            return;
        std::ostringstream stem;
        stem << "gfx_" << std::hex << std::setfill('0') << std::setw(16) << guest_hash << '_'
             << std::dec << stage;
        if (!variants.empty())
            stem << '_' << std::hex << std::setw(16) << source_hash;
        std::ofstream hlsl{directory / (stem.str() + ".hlsl"), std::ios::binary};
        std::ofstream hashes{directory / (stem.str() + ".hash"), std::ios::binary};
        hlsl << source;
        hashes << "guest_hash=" << std::hex << std::setw(16) << std::setfill('0') << guest_hash
               << " source_hash=" << std::setw(16) << source_hash << " stage=" << std::dec << stage
               << '\n';
        hlsl.close();
        hashes.close();
        if (!hlsl || !hashes)
            throw std::runtime_error("Cannot write HLSL dump");
        variants.insert(source_hash);
    }

private:
    std::mutex mutex;
    std::map<std::pair<std::uint64_t, int>, std::set<std::uint64_t>> written;
};
} // namespace D3D12
