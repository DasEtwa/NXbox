// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "video_core/renderer_d3d12/d3d12_record_args.h"

namespace D3D12 {
struct RecordedRootBinding {
    std::uint64_t address;
    std::uint32_t parameter;
    std::uint32_t table;
};

// Snapshot only changed bindings into one stream command. Root arguments contain
// GPU addresses, not COM objects; resource/heap lifetimes stay with the submission.
// Target is dependent so the same replay runs against host test command lists.
template <class Target, class Stream>
void RecordGraphicsRootBindings(Stream& stream, const RecordedRootBinding* bindings, size_t count) {
    if (!count)
        return;
    EncodeCall<[](void* target, const RecordedRootBinding* values, size_t size) {
        auto* const list = static_cast<Target*>(target)->list;
        for (size_t i = 0; i < size; ++i) {
            const auto& binding = values[i];
            if (binding.table)
                list->SetGraphicsRootDescriptorTable(binding.parameter, {binding.address});
            else
                list->SetGraphicsRootConstantBufferView(binding.parameter, binding.address);
        }
    }>(stream, Snapshot(bindings, count), count);
}

// Compare object representations: floating-point state must preserve signed zero
// and NaN payloads. Only use this with initialized, trivially copyable API values.
template <class T>
struct RecordedValue {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    bool valid{};
    bool Update(const T& next) {
        if (valid && std::memcmp(&value, &next, sizeof(T)) == 0)
            return false;
        std::memcpy(&value, &next, sizeof(T));
        valid = true;
        return true;
    }
};

template <class T, size_t Capacity>
struct RecordedArray {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<T, Capacity> values{};
    size_t count{};
    bool valid{};
    bool Update(const T* next, size_t next_count) {
        // An oversized future caller must record rather than truncate its state.
        if (next_count > Capacity) {
            valid = false;
            return true;
        }
        if (valid && count == next_count &&
            (next_count == 0 || std::memcmp(values.data(), next, next_count * sizeof(T)) == 0))
            return false;
        if (next_count != 0)
            std::memcpy(values.data(), next, next_count * sizeof(T));
        count = next_count;
        valid = true;
        return true;
    }
};
// Root descriptors are addressed by parameter index. Keep validity in one word
// instead of clearing every cached address on a root-signature/heap change.
template <class T, size_t Capacity>
struct RecordedSlots {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(Capacity <= 64);
    std::array<T, Capacity> values{};
    std::uint64_t valid{};

    bool Update(size_t slot, const T& next) {
        if (slot >= Capacity) {
            valid = 0;
            return true;
        }
        const auto bit = std::uint64_t{1} << slot;
        if ((valid & bit) && std::memcmp(&values[slot], &next, sizeof(T)) == 0)
            return false;
        std::memcpy(&values[slot], &next, sizeof(T));
        valid |= bit;
        return true;
    }
};

// Independent of conservative draw-state invalidation. Only command-list reset,
// a different root signature, or a write to the same root parameter changes CBVs.
struct GraphicsRootCbvCache {
    RecordedValue<std::uintptr_t> signature;
    RecordedSlots<std::uint64_t, 64> cbvs;
    void Reset() {
        signature.valid = false;
        cbvs.valid = 0;
    }
    void SetSignature(std::uintptr_t next) {
        if (signature.Update(next))
            cbvs.valid = 0;
    }
    void Invalidate(std::size_t slot) {
        if (slot < 64)
            cbvs.valid &= ~(std::uint64_t{1} << slot);
        else
            cbvs.valid = 0;
    }
};

} // namespace D3D12

#ifdef _WIN32
#include <d3d12.h>

namespace D3D12 {
// Owned by Context, never shared across command lists. All arrays are bounded by
// D3D12 API limits; recording state performs no allocations.
struct RecordedDrawState {
    std::uint64_t generation{};
    RecordedValue<std::array<ID3D12DescriptorHeap*, 2>> heaps;
    RecordedValue<ID3D12RootSignature*> root_signature;
    RecordedValue<ID3D12PipelineState*> pipeline;
    RecordedValue<D3D_PRIMITIVE_TOPOLOGY> topology;
    struct RootArguments {
        RecordedSlots<D3D12_GPU_VIRTUAL_ADDRESS, 64> tables;
        RecordedSlots<D3D12_GPU_VIRTUAL_ADDRESS, 64> cbvs;
        // Guest graphics has a single constants parameter, at root index zero.
        RecordedArray<std::uint32_t, 16> constants;
    } root_args;
    // Count, contiguous RTV base, DSV handle, DSV presence. The rasterizer also
    // invalidates on fixed RTV/DSV descriptor rewrites, even if handles match.
    RecordedValue<std::array<std::uint64_t, 4>> targets;
    RecordedArray<D3D12_VIEWPORT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports;
    RecordedArray<D3D12_RECT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors;
    RecordedArray<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices;
    RecordedValue<D3D12_INDEX_BUFFER_VIEW> index;
    RecordedValue<std::array<UINT, 3>> stencil;
    RecordedValue<std::array<float, 4>> blend;
    RecordedValue<std::array<float, 2>> depth_bounds;
    RecordedArray<D3D12_STREAM_OUTPUT_BUFFER_VIEW, 4> stream_output;

    void Refresh(std::uint64_t current_generation, bool full_reset = false) {
        if (generation != current_generation) {
            if (full_reset) {
                *this = {};
            } else {
                // Values are unreadable until Update sets validity again. Preserve
                // payloads so per-draw invalidation writes only validity metadata.
                heaps.valid = false;
                root_signature.valid = false;
                pipeline.valid = false;
                topology.valid = false;
                targets.valid = false;
                viewports.valid = false;
                scissors.valid = false;
                vertices.valid = false;
                index.valid = false;
                stencil.valid = false;
                blend.valid = false;
                depth_bounds.valid = false;
                stream_output.valid = false;
                ClearRootArguments();
            }
            generation = current_generation;
        }
    }
    void ClearRootArguments() {
        root_args.tables.valid = 0;
        root_args.cbvs.valid = 0;
        root_args.constants.valid = false;
    }
};
} // namespace D3D12
#endif // _WIN32
