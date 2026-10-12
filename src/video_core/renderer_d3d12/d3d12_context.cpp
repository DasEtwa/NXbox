// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_context.h"

#include <algorithm>
#include <stdexcept>
#include <d3d12sdklayers.h>
#include <fmt/format.h>
#include <windows.h>
#ifdef NXBOX_UWP
#include "eden_uwp/diagnostic.h"
#endif
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_completion.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"

namespace D3D12 {
void Diagnostic(std::string_view message) noexcept {
    try {
        LOG_ERROR(Render, "{}", message);
#ifdef NXBOX_UWP
        EdenXbox::Diagnostic(std::string{message});
        const std::filesystem::path folder{
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()};
        static std::mutex mutex;
        const std::lock_guard lock{mutex};
        const HANDLE file =
            CreateFile2FromAppW((folder / "nxbox_crash.txt").c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const std::string line = std::string{message} + "\n";
            OVERLAPPED append{};
            append.Offset = append.OffsetHigh = MAXDWORD;
            DWORD written{};
            WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, &append);
            FlushFileBuffers(file);
            CloseHandle(file);
        }
#endif
    } catch (...) {
        OutputDebugStringA("D3D12 diagnostic unavailable\n");
    }
}
namespace {
void LogDeviceInfoQueue(ID3D12Device* device) noexcept {
    try {
        if (!device)
            return;
        char debug[2]{};
        if (GetEnvironmentVariableA("NXBOX_D3D12_DEBUG", debug, sizeof(debug)) != 1 ||
            debug[0] != '1')
            return;
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info))))
            return;
        const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size{};
            if (FAILED(info->GetMessage(i, nullptr, &size)))
                continue;
            std::vector<u8> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &size)))
                Diagnostic(fmt::format("D3D12 InfoQueue severity={} id={} {}",
                                       static_cast<u32>(message->Severity),
                                       static_cast<u32>(message->ID), message->pDescription));
        }
        info->ClearStoredMessages();
    } catch (...) {
        Diagnostic("D3D12 InfoQueue logging failed");
    }
}
void LogDeviceDred(ID3D12Device* device) noexcept {
    try {
        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        const auto status = device ? device->QueryInterface(IID_PPV_ARGS(&dred)) : E_POINTER;
        if (FAILED(status)) {
            Diagnostic(
                fmt::format("D3D12 DRED unavailable HRESULT={:#010x}", static_cast<u32>(status)));
        } else {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
            const auto result = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
            Diagnostic(
                fmt::format("D3D12 DRED breadcrumbs HRESULT={:#010x}", static_cast<u32>(result)));
            size_t nodes{};
            for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode;
                 SUCCEEDED(result) && node && nodes++ < 256; node = node->pNext) {
                const auto last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                Diagnostic(fmt::format(
                    "D3D12 DRED list={} queue={} completed={}/{}",
                    node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "unnamed",
                    node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "unnamed", last,
                    node->BreadcrumbCount));
                if (node->pCommandHistory) {
                    const UINT begin = last > 8 ? last - 8 : 0;
                    const UINT end = std::min(node->BreadcrumbCount, last + 8);
                    for (UINT i = begin; i < end; ++i)
                        Diagnostic(fmt::format("D3D12 DRED op[{}]={}", i,
                                               static_cast<u32>(node->pCommandHistory[i])));
                }
            }
            D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
            const auto fault_status = dred->GetPageFaultAllocationOutput(&fault);
            Diagnostic(fmt::format("D3D12 DRED page fault HRESULT={:#010x} address={:#x}",
                                   static_cast<u32>(fault_status), fault.PageFaultVA));
            if (SUCCEEDED(fault_status)) {
                const auto allocations = [](const char* kind,
                                            const D3D12_DRED_ALLOCATION_NODE* node) {
                    size_t count{};
                    for (; node && count++ < 256; node = node->pNext)
                        Diagnostic(fmt::format("D3D12 DRED {} allocation={} type={}", kind,
                                               node->ObjectNameA ? node->ObjectNameA : "unnamed",
                                               static_cast<u32>(node->AllocationType)));
                };
                allocations("existing", fault.pHeadExistingAllocationNode);
                allocations("freed", fault.pHeadRecentFreedAllocationNode);
            }
        }
    } catch (...) {
        Diagnostic("D3D12 DRED logging failed");
    }
}
} // namespace
void Check(HRESULT result, const char* operation, ID3D12Device* device) {
    if (FAILED(result)) {
        const auto message = fmt::format(
            "D3D12 {} HRESULT={:#010x} removed={}", operation, static_cast<u32>(result),
            device ? fmt::format("{:#010x}", static_cast<u32>(device->GetDeviceRemovedReason()))
                   : "unavailable");
        Diagnostic(message);
        LogDeviceInfoQueue(device);
        LogDeviceDred(device);
        throw std::runtime_error(message);
    }
}
void CheckPipeline(HRESULT result, const char* operation, ID3D12Device* device, u64 hash) {
    if (FAILED(result))
        Diagnostic(fmt::format("D3D12 PSO hash={:016x} {} HRESULT={:#010x}", hash, operation,
                               static_cast<u32>(result)));
    Check(result, operation, device);
}
Resource::~Resource() {
    if (!mapped.empty())
        handle->Unmap(0, nullptr);
    if (budget)
        budget->used -= allocation_size;
}
Context::Context() {
    SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", nullptr);
    SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", nullptr);
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    const auto dred_status = D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
    if (SUCCEEDED(dred_status)) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        Diagnostic("D3D12 DRED enabled before device creation");
    } else {
        Diagnostic(fmt::format("D3D12 DRED settings unavailable HRESULT={:#010x}",
                               static_cast<u32>(dred_status)));
    }
    char debug[2]{};
    if (GetEnvironmentVariableA("NXBOX_D3D12_DEBUG", debug, sizeof(debug)) == 1 &&
        debug[0] == '1') {
        ComPtr<ID3D12Debug> layer;
        const auto status = D3D12GetDebugInterface(IID_PPV_ARGS(&layer));
        if (SUCCEEDED(status)) {
            layer->EnableDebugLayer();
            Diagnostic("D3D12 debug layer enabled");
        } else {
            Diagnostic(fmt::format("D3D12 debug layer unavailable HRESULT={:#010x}",
                                   static_cast<u32>(status)));
        }
    }
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "CreateDevice");
    if (debug[0] == '1') {
        ComPtr<ID3D12InfoQueue> info;
        const auto status = device.As(&info);
        Diagnostic(fmt::format("D3D12 InfoQueue {} HRESULT={:#010x}",
                               SUCCEEDED(status) ? "available" : "unavailable",
                               static_cast<u32>(status)));
    }
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue",
          device.Get());
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence",
          device.Get());
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator", device.Get());
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)),
          "CreateCommandList", device.Get());
    Check(commands->Close(), "initial Close", device.Get());
    event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!event)
        Check(HRESULT_FROM_WIN32(GetLastError()), "CreateEventExW", device.Get());
}
Context::~Context() {
    try {
        Finish();
    } catch (const std::exception& error) {
        ReportFailure(error);
    }
    if (event)
        CloseHandle(event);
}
void Context::LogInfoQueue() noexcept {
    LogDeviceInfoQueue(device.Get());
}
void Context::ReportFailure(const std::exception& error) noexcept {
    try {
        Diagnostic(
            fmt::format("D3D12 boundary what={} removed={:#010x}", error.what(),
                        static_cast<u32>(device ? device->GetDeviceRemovedReason() : E_POINTER)));
        LogInfoQueue();
        LogDeviceDred(device.Get());
        SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", error.what());
        SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", "1");
    } catch (...) {
        SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", "1");
        Diagnostic("D3D12 failure reporting incomplete");
    }
}
bool Context::IsFree(u64 tick) const {
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        Check(device->GetDeviceRemovedReason(), "GetCompletedValue device removed", device.Get());
        throw std::runtime_error("D3D12 fence returned UINT64_MAX with S_OK removed reason");
    }
    return completed >= tick;
}
void Context::Wait(u64 tick) {
    if (tick > submitted) {
        if (tick != CurrentTick())
            throw std::invalid_argument("D3D12 future fence");
        Submit();
    }
    if (!IsFree(tick)) {
        Check(fence->SetEventOnCompletion(tick, event), "SetEventOnCompletion", device.Get());
        const auto wait = WaitForSingleObjectEx(event, 10000, FALSE);
        if (wait != WAIT_OBJECT_0) {
            const auto wait_error = GetLastError();
            Diagnostic(
                fmt::format("D3D12 wait tick={} submitted={} result={:#x}", tick, submitted, wait));
            Check(device->GetDeviceRemovedReason(), "device removed while waiting", device.Get());
            Check(wait == WAIT_FAILED ? HRESULT_FROM_WIN32(wait_error)
                                      : HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                  "WaitForSingleObjectEx", device.Get());
            throw std::runtime_error("D3D12 fence wait failed/timed out");
        }
    }
    Check(device->GetDeviceRemovedReason(), "device removed after waiting", device.Get());
    LogInfoQueue();
    if (tick == submitted && !recording) {
        CompleteBatch(completion, retained);
    }
}
ID3D12GraphicsCommandList* Context::Commands() {
    if (!recording) {
        Wait(submitted);
    }
    // A completion callback may have started the next command list.
    if (!recording) {
        Check(allocator->Reset(), "allocator Reset", device.Get());
        Check(commands->Reset(allocator.Get(), nullptr), "command list Reset", device.Get());
        recording = true;
    }
    return commands.Get();
}
u64 Context::Submit() {
    if (recording) {
        LogInfoQueue();
        Check(commands->Close(), "Close", device.Get());
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        recording = false;
    }
    Check(queue->Signal(fence.Get(), ++submitted), "Signal", device.Get());
    LogInfoQueue();
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
          "CreateCommittedResource", device.Get());
    resource->heap = heap;
    resource->state = state;
    resource->allocation_size = allocation.SizeInBytes;
    resource->budget = budget;
    budget->used += allocation.SizeInBytes;
    if (heap != D3D12_HEAP_TYPE_DEFAULT) {
        void* mapped{};
        D3D12_RANGE read_range{0, heap == D3D12_HEAP_TYPE_READBACK ? static_cast<SIZE_T>(desc.Width)
                                                                   : 0};
        Check(resource->handle->Map(0, &read_range, &mapped), "Map staging", device.Get());
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
    // P3: guest storage buffers are bound as raw UAVs.
    if (heap == D3D12_HEAP_TYPE_DEFAULT)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
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
