// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_context.h"

#include <algorithm>
#include <stdexcept>
#include <windows.h>
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"

namespace D3D12 {
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        LOG_ERROR(Render, "D3D12 {} failed: {:#x}", operation, static_cast<u32>(result));
        throw std::runtime_error(std::string("D3D12 failure: ") + operation);
    }
}
Resource::~Resource() {
    if (!mapped.empty())
        handle->Unmap(0, nullptr);
    if (budget)
        budget->used -= allocation_size;
}
Context::Context() {
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "CreateDevice");
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)),
          "CreateCommandList");
    Check(commands->Close(), "initial Close");
    event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!event)
        Check(HRESULT_FROM_WIN32(GetLastError()), "CreateEventExW");
}
Context::~Context() {
    try {
        Finish();
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 teardown: {}", error.what());
    }
    if (event)
        CloseHandle(event);
}
bool Context::IsFree(u64 tick) const {
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        throw std::runtime_error("D3D12 device removed");
    return completed >= tick;
}
void Context::Wait(u64 tick) {
    if (tick > submitted) {
        if (tick != CurrentTick())
            throw std::invalid_argument("D3D12 future fence");
        Submit();
    }
    if (!IsFree(tick)) {
        Check(fence->SetEventOnCompletion(tick, event), "SetEventOnCompletion");
        if (WaitForSingleObjectEx(event, 10000, FALSE) != WAIT_OBJECT_0) {
            Check(device->GetDeviceRemovedReason(), "device removed while waiting");
            throw std::runtime_error("D3D12 fence wait failed/timed out");
        }
    }
    Check(device->GetDeviceRemovedReason(), "device removed after waiting");
    if (tick == submitted && !recording) {
        for (auto& callback : completion)
            callback();
        completion.clear();
        retained.clear();
    }
}
ID3D12GraphicsCommandList* Context::Commands() {
    if (!recording) {
        Wait(submitted);
        Check(allocator->Reset(), "allocator Reset");
        Check(commands->Reset(allocator.Get(), nullptr), "command list Reset");
        recording = true;
    }
    return commands.Get();
}
u64 Context::Submit() {
    if (recording) {
        Check(commands->Close(), "Close");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        recording = false;
    }
    Check(queue->Signal(fence.Get(), ++submitted), "Signal");
    return submitted;
}
void Context::Finish() {
    Wait(Submit());
}
void Context::Retain(const ResourcePtr& resource) {
    Commands();
    retained.push_back(resource);
}
void Context::Transition(const ResourcePtr& resource, D3D12_RESOURCE_STATES state) {
    if (!resource)
        throw std::invalid_argument("D3D12 null resource");
    Retain(resource);
    if (resource->heap != D3D12_HEAP_TYPE_DEFAULT || resource->state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource->handle.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = resource->state;
    barrier.Transition.StateAfter = state;
    commands->ResourceBarrier(1, &barrier);
    resource->state = state;
}
void Context::AfterCompletion(std::function<void()> callback) {
    Commands();
    completion.push_back(std::move(callback));
}
ResourcePtr Context::CreateResource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_STATES state) {
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
    if (allocation.SizeInBytes == UINT64_MAX ||
        allocation.SizeInBytes > MemoryBudget::Limit - budget->used)
        throw std::runtime_error("D3D12 cache allocation exceeds 512 MiB budget");
    auto resource = std::make_shared<Resource>();
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource->handle)),
          "CreateCommittedResource");
    resource->heap = heap;
    resource->state = state;
    resource->allocation_size = allocation.SizeInBytes;
    resource->budget = budget;
    budget->used += allocation.SizeInBytes;
    if (heap != D3D12_HEAP_TYPE_DEFAULT) {
        void* mapped{};
        D3D12_RANGE read_range{0, heap == D3D12_HEAP_TYPE_READBACK ? static_cast<SIZE_T>(desc.Width)
                                                                   : 0};
        Check(resource->handle->Map(0, &read_range, &mapped), "Map staging");
        resource->mapped = {static_cast<u8*>(mapped), static_cast<size_t>(desc.Width)};
    }
    return resource;
}
ResourcePtr Context::CreateBuffer(u64 size, D3D12_HEAP_TYPE heap) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max(u64{1}, size);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto state = heap == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                       : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                          : D3D12_RESOURCE_STATE_COMMON;
    return CreateResource(desc, heap, state);
}
void StagingRing::Collect() {
    for (auto& page : pages) {
        if (context.IsFree(page.tick) && page.resource.use_count() == 1)
            page.cursor = 0;
    }
}
StagingBufferRef StagingRing::Request(size_t size) {
    if (!size)
        throw std::invalid_argument("D3D12 empty staging allocation");
    Collect();
    const u64 aligned = Align(size, 512);
    for (auto& page : pages) {
        if (aligned <= page.resource->mapped.size() - page.cursor) {
            const auto offset = page.cursor;
            page.cursor += aligned;
            page.tick = context.CurrentTick();
            return {page.resource, static_cast<size_t>(offset),
                    page.resource->mapped.subspan(offset, size)};
        }
    }
    u64 capacity{};
    for (const auto& page : pages)
        capacity += page.resource->mapped.size();
    const u64 page_size = std::max(4ULL << 20, aligned);
    if (page_size > (64ULL << 20) - capacity)
        throw std::runtime_error("D3D12 staging ring exceeds 64 MiB; submit pending work");
    pages.push_back({context.CreateBuffer(page_size, heap), aligned, context.CurrentTick()});
    return {pages.back().resource, 0, pages.back().resource->mapped.first(size)};
}
} // namespace D3D12
