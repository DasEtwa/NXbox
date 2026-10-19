// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
namespace Tegra {
class MemoryManager;
}
namespace VideoCommon::ShaderDiscovery {
bool Enabled();
void Reset();
void Base(std::uint64_t space, std::uint64_t address, bool compute);
// Copies immediately into bounded owned storage; never retains the input span.
void OwnedUpload(std::uint64_t space, std::uint64_t address, std::span<const std::uint8_t> bytes,
                 bool bulk = false);
void Demand(std::uint64_t space, std::uint64_t address, std::uint64_t hash, unsigned slot,
            bool compute);
void UnsupportedHash(std::uint64_t space, std::uint64_t hash);
void EndFrame(Tegra::MemoryManager* memory = nullptr);
void Shutdown();
} // namespace VideoCommon::ShaderDiscovery
