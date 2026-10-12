// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include "common/common_types.h"

namespace D3D12 {
using Microsoft::WRL::ComPtr;
void Diagnostic(std::string_view message) noexcept;
bool BisectEnabled(const char* name) noexcept;
void Check(HRESULT result, const char* operation, ID3D12Device* device = nullptr);
void CheckPipeline(HRESULT result, const char* operation, ID3D12Device* device, u64 hash);
struct MemoryBudget {
    u64 used{};
    static constexpr u64 Limit = 512ULL << 20;
};
struct Resource {
    ComPtr<ID3D12Resource> handle;
    D3D12_RESOURCE_STATES state{};
    D3D12_HEAP_TYPE heap{};
    std::span<u8> mapped;
    std::shared_ptr<MemoryBudget> budget;
    u64 allocation_size{};
    ~Resource();
};
using ResourcePtr = std::shared_ptr<Resource>;
struct StagingBufferRef {
    ResourcePtr buffer;
    size_t offset{};
    std::span<u8> mapped_span;
};
// One direct queue, serialized allocators. Every referenced resource is retained
// until completion, including resources evicted from the shared caches.
class Context {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ID3D12GraphicsCommandList* Commands();
    u64 Submit();
    void MarkWork(std::string_view label);
    void Finish();
    void ReportFailure(const std::exception& error) noexcept;
    void LogInfoQueue() noexcept;
    void Wait(u64 tick);
    bool IsFree(u64 tick) const;
    u64 CurrentTick() const {
        return submitted + 1;
    }
    void Transition(const ResourcePtr& resource, D3D12_RESOURCE_STATES state);
    void Retain(const ResourcePtr& resource);
    void AfterCompletion(std::function<void()> callback);
    ResourcePtr CreateResource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
                               D3D12_RESOURCE_STATES state);
    ResourcePtr CreateBuffer(u64 size, D3D12_HEAP_TYPE heap);
    ID3D12Device* Device() const {
        return device.Get();
    }
    ID3D12CommandQueue* Queue() const {
        return queue.Get();
    }
    u64 MemoryUsage() const {
        return budget->used;
    }

private:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    struct InfoCallback {
        ComPtr<ID3D12InfoQueue1> queue;
        DWORD cookie{};
        bool registered{};
        ~InfoCallback() {
            if (registered)
                queue->UnregisterMessageCallback(cookie);
        }
    } info_callback;
    u64 execute_sequence{};
    std::vector<std::string> work;
    HANDLE event{};
    u64 submitted{};
    bool recording{};
    std::shared_ptr<MemoryBudget> budget = std::make_shared<MemoryBudget>();
    std::vector<ResourcePtr> retained;
    std::vector<std::function<void()>> completion;
};
class StagingRing {
public:
    StagingRing(Context& context, D3D12_HEAP_TYPE heap) : context{context}, heap{heap} {}
    StagingBufferRef Request(size_t size);
    void Collect();

private:
    struct Page {
        ResourcePtr resource;
        u64 cursor{};
        u64 tick{};
    };
    Context& context;
    D3D12_HEAP_TYPE heap;
    std::vector<Page> pages;
};
} // namespace D3D12
