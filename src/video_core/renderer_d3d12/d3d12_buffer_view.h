// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace D3D12 {
inline void ValidateBufferView(std::uint64_t address, std::uint64_t capacity, std::uint64_t offset,
                               std::uint64_t size) {
    if (!address || !size || offset > capacity || size > capacity - offset ||
        offset > std::numeric_limits<std::uint64_t>::max() - address ||
        size > std::numeric_limits<std::uint64_t>::max() - address - offset)
        throw std::out_of_range("D3D12 invalid geometry buffer view");
}
} // namespace D3D12
