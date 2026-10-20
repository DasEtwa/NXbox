// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace D3D12 {
// CPU RTV/DSV handles are consumed during replay, not by the GPU. Fixed attachment
// descriptors can be rewritten on the producer immediately after encoding a draw.
// Snapshot into bounded, recorder-retired slots without changing descriptor tables.
class RecordDescriptors {
public:
    static constexpr size_t Capacity = 4096;
    void Initialize(ID3D12Device* value) {
        device = value;
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = Capacity * 8;
        Check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtvs)), "recorder RTV snapshots",
              device);
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = Capacity;
        Check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsvs)), "recorder DSV snapshots",
              device);
        rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        rtv_base = rtvs->GetCPUDescriptorHandleForHeapStart();
        dsv_base = dsvs->GetCPUDescriptorHandleForHeapStart();
    }
    size_t Acquire(RecordStream<>& stream) {
        const auto index = next++ % Capacity;
        if (busy[index].load(std::memory_order_acquire)) {
            stream.Flush();
            stream.BoundaryWait([&] {
                while (busy[index].load(std::memory_order_acquire))
                    busy[index].wait(true, std::memory_order_acquire);
            });
        }
        busy[index].store(true, std::memory_order_relaxed);
        return index;
    }
    void Release(size_t index) {
        busy[index].store(false, std::memory_order_release);
        busy[index].notify_one();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(size_t slot, UINT index = 0) const {
        return {rtv_base.ptr + (slot * 8 + index) * rtv_stride};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(size_t slot) const {
        return {dsv_base.ptr + slot * dsv_stride};
    }
    void Copy(D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source,
              bool depth) {
        device->CopyDescriptorsSimple(1, destination, source,
                                      depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV
                                            : D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    UINT RtvStride() const {
        return rtv_stride;
    }

private:
    ID3D12Device* device{};
    ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
    UINT rtv_stride{}, dsv_stride{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_base{}, dsv_base{};
    size_t next{};
    std::array<std::atomic<bool>, Capacity> busy{};
};
struct DescriptorTicket {
    RecordDescriptors* pool;
    size_t index;
};
template <>
struct RecordLease<DescriptorTicket> {
    static void Hold(DescriptorTicket) {}
    static void Drop(DescriptorTicket ticket) {
        ticket.pool->Release(ticket.index);
    }
};
} // namespace D3D12
