// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/renderer_d3d12.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"

namespace D3D12 {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        LOG_ERROR(Render, "D3D12 {} failed: {:#x}", operation, static_cast<u32>(result));
        throw std::runtime_error(std::string("D3D12 failure: ") + operation);
    }
}
} // namespace

struct RendererD3D12::Impl {
    static constexpr UINT BufferCount = 2;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    std::array<ComPtr<ID3D12Resource>, BufferCount> buffers;
    HANDLE fence_event{};
    u64 fence_value{};
    UINT rtv_stride{};
    u32 width{};
    u32 height{};
    bool failed{};

    ~Impl() {
        // Also covers partial renderer construction after a submission/presentation failure.
        if (queue && fence && fence_event) {
            try {
                Drain();
            } catch (const std::exception& error) {
                LOG_ERROR(Render, "D3D12 teardown: {}", error.what());
            }
        }
        if (fence_event) {
            CloseHandle(fence_event);
        }
    }

    void Wait() {
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) {
            Check(device->GetDeviceRemovedReason(), "device removed");
            throw std::runtime_error("D3D12 fence reported device removal");
        }
        if (completed >= fence_value) {
            return;
        }
        Check(fence->SetEventOnCompletion(fence_value, fence_event), "SetEventOnCompletion");
        const DWORD result = WaitForSingleObjectEx(fence_event, 10000, FALSE);
        if (result != WAIT_OBJECT_0) {
            Check(device->GetDeviceRemovedReason(), "device removed while waiting");
            LOG_ERROR(Render, "D3D12 fence wait failed/timed out: {}", result);
            throw std::runtime_error("D3D12 fence wait failed/timed out");
        }
        Check(device->GetDeviceRemovedReason(), "device removed after waiting");
    }

    void Drain() {
        Check(queue->Signal(fence.Get(), ++fence_value), "Signal");
        Wait();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index) const {
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtv_stride;
        return handle;
    }

    void AcquireBuffers() {
        for (UINT i = 0; i < BufferCount; ++i) {
            Check(swapchain->GetBuffer(i, IID_PPV_ARGS(buffers[i].ReleaseAndGetAddressOf())),
                  "GetBuffer");
            device->CreateRenderTargetView(buffers[i].Get(), nullptr, Rtv(i));
        }
    }

    void Initialize(Core::Frontend::EmuWindow& window) {
        const auto& info = window.GetWindowInfo();
        if (info.type != Core::Frontend::WindowSystemType::Windows || !info.render_surface) {
            throw std::runtime_error("D3D12 requires a UWP CoreWindow ABI surface");
        }
        Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
              "CreateDevice");
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        fence_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (!fence_event) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "CreateEventExW");
        }
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator)),
              "CreateCommandAllocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands)),
              "CreateCommandList");
        Check(commands->Close(), "initial Close");

        ComPtr<IDXGIFactory2> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        width = std::max(1u, window.GetFramebufferLayout().width);
        height = std::max(1u, window.GetFramebufferLayout().height);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = BufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> chain;
        Check(factory->CreateSwapChainForCoreWindow(
                  queue.Get(), static_cast<IUnknown*>(info.render_surface), &desc, nullptr, &chain),
              "CreateSwapChainForCoreWindow");
        Check(chain.As(&swapchain), "Query IDXGISwapChain3");
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = BufferCount;
        Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)),
              "CreateDescriptorHeap RTV");
        rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        AcquireBuffers();
        LOG_WARNING(Render, "D3D12 P1 initialized: native clear/present, no guest rendering");
    }

    void Present(u32 new_width, u32 new_height) {
        // P1 deliberately serializes frames. TODO(P2): fence-retired per-frame allocators/staging.
        Wait();
        new_width = std::max(1u, new_width);
        new_height = std::max(1u, new_height);
        if (width != new_width || height != new_height) {
            Drain();
            for (auto& buffer : buffers) {
                buffer.Reset();
            }
            Check(swapchain->ResizeBuffers(BufferCount, new_width, new_height,
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0),
                  "ResizeBuffers");
            width = new_width;
            height = new_height;
            AcquireBuffers();
        }
        Check(allocator->Reset(), "allocator Reset");
        Check(commands->Reset(allocator.Get(), nullptr), "command list Reset");
        const UINT index = swapchain->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffers[index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands->ResourceBarrier(1, &barrier);
        const auto target = Rtv(index);
        commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
        constexpr float TestColor[] = {0.08f, 0.25f, 0.5f, 1.0f};
        commands->ClearRenderTargetView(target, TestColor, 0, nullptr);
        // TODO(P2): look up/decode the guest framebuffer with VideoCommon::TextureCache.
        // No cache resource exists in P1, so copying a guest texture is not yet possible.
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
        Check(commands->Close(), "Close");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        // Signal even if Present fails, so teardown can retire submitted work.
        Check(queue->Signal(fence.Get(), ++fence_value), "frame Signal");
        Check(swapchain->Present(1, 0), "Present");
    }
};

RendererD3D12::RendererD3D12(Core::Frontend::EmuWindow& window, Tegra::GPU& gpu_,
                             std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : RendererBase(window, std::move(context_)), impl(std::make_unique<Impl>()), gpu(gpu_),
      rasterizer(gpu_) {
    impl->Initialize(window);
    // Show the probe even when the guest has not submitted a framebuffer yet.
    impl->Present(window.GetFramebufferLayout().width, window.GetFramebufferLayout().height);
    impl->Wait();
}

RendererD3D12::~RendererD3D12() = default;

void RendererD3D12::Composite(std::span<const Tegra::FramebufferConfig> layers) {
    if (!layers.empty() && !framebuffer_logged) {
        LOG_WARNING(Render, "D3D12 P1: guest framebuffer ignored (TODO P2 texture cache)");
        framebuffer_logged = true;
    }
    if (IsScreenshotPending()) {
        renderer_settings.screenshot_requested = false;
        // The callback bool describes row inversion, not success. Return a blank capture.
        const auto& size = renderer_settings.screenshot_framebuffer_layout;
        std::fill_n(static_cast<u8*>(renderer_settings.screenshot_bits),
                    static_cast<size_t>(size.width) * size.height * 4, u8{});
        LOG_WARNING(Render, "D3D12 P1: screenshot readback is stubbed (TODO P2)");
        renderer_settings.screenshot_complete_callback(false);
    }
    if (!impl->failed && render_window.IsShown()) {
        const auto& layout = render_window.GetFramebufferLayout();
        try {
            impl->Present(layout.width, layout.height);
            ++m_current_frame;
            render_window.OnFrameDisplayed();
        } catch (const std::exception& error) {
            impl->failed = true;
            LOG_ERROR(Render, "D3D12 presentation stopped: {}", error.what());
            // The frontend observes this marker and shuts down the session on its worker thread.
            SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", "1");
            SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", error.what());
        }
    }
    gpu.RendererFrameEndNotify();
}

std::vector<u8> RendererD3D12::GetAppletCaptureBuffer() {
    // TODO(P2): capture guest layers instead of returning an empty, correctly sized buffer.
    return std::vector<u8>(VideoCore::Capture::TiledSize);
}

VideoCore::RasterizerInterface* RendererD3D12::ReadRasterizer() {
    return &rasterizer;
}

std::string RendererD3D12::GetDeviceVendor() const {
    return "D3D12 (P1)";
}
} // namespace D3D12
