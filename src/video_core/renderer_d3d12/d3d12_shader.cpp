// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdexcept>
#include <fmt/format.h>
#include <windows.h>
#include <wrl/client.h>
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_compile_policy.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12 {
namespace {
// Minimal public DXC COM ABI, from Microsoft DirectXShaderCompiler's dxcapi.h.
// Keep the vtable order and GUIDs identical; no import library or Mesa dependency.
struct DxcBlob : IUnknown {
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
struct DxcBlobEncoding : DxcBlob {
    virtual HRESULT STDMETHODCALLTYPE GetEncoding(BOOL*, UINT32*) = 0;
};
struct DxcOperationResult : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResult(DxcBlob**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(DxcBlobEncoding**) = 0;
};
struct DxcBuffer {
    const void* Ptr;
    SIZE_T Size;
    UINT Encoding;
};
struct DxcCompiler3 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Compile(const DxcBuffer*, LPCWSTR*, UINT32, IUnknown*, REFIID,
                                              void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disassemble(const DxcBuffer*, REFIID, void**) = 0;
};
constexpr GUID CompilerClass{
    0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
constexpr GUID CompilerInterface{
    0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
constexpr GUID ResultInterface{
    0xcedb484a, 0xd4e9, 0x445a, {0xb9, 0x91, 0xca, 0x21, 0xca, 0x15, 0x7d, 0xc2}};
} // namespace
struct ShaderCompiler::Impl {
    HMODULE library{};
    ComPtr<DxcCompiler3> compiler;
    ~Impl() {
        compiler.Reset();
        if (library)
            FreeLibrary(library);
    }
};
ShaderCompiler::ShaderCompiler() : impl{std::make_unique<Impl>()} {
#ifdef NXBOX_UWP
    impl->library = LoadPackagedLibrary(L"dxcompiler.dll", 0);
    if (!impl->library)
        Check(HRESULT_FROM_WIN32(GetLastError()), "LoadPackagedLibrary dxcompiler.dll");
#else
    // Resolve beside the executable even when launched from another working directory.
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw std::runtime_error("D3D12 cannot resolve executable directory");
    path.resize(length);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"dxcompiler.dll";
    impl->library = LoadLibraryExW(
        path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!impl->library)
        Check(HRESULT_FROM_WIN32(GetLastError()), "LoadLibraryExW dxcompiler.dll");
#endif
    using CreateInstance = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    const auto create =
        reinterpret_cast<CreateInstance>(GetProcAddress(impl->library, "DxcCreateInstance"));
    if (!create)
        throw std::runtime_error("D3D12 missing DxcCreateInstance");
    Check(create(CompilerClass, CompilerInterface,
                 reinterpret_cast<void**>(impl->compiler.GetAddressOf())),
          "DxcCreateInstance");
}
ShaderCompiler::~ShaderCompiler() = default;
std::string ShaderCompiler::Disassemble(std::span<const u8> dxil) {
    DxcBuffer buffer{dxil.data(), dxil.size(), 0};
    ComPtr<DxcOperationResult> result;
    Check(impl->compiler->Disassemble(&buffer, ResultInterface,
                                      reinterpret_cast<void**>(result.GetAddressOf())),
          "DXC Disassemble");
    HRESULT status{};
    Check(result->GetStatus(&status), "DXC disassembly status");
    Check(status, "DXC disassembly");
    ComPtr<DxcBlob> blob;
    Check(result->GetResult(&blob), "DXC disassembly result");
    return blob ? std::string{static_cast<const char*>(blob->GetBufferPointer()),
                              blob->GetBufferSize()}
                : std::string{};
}
std::vector<u8> ShaderCompiler::Compile(std::string_view source, const wchar_t* entry,
                                        const wchar_t* target, u64 pipeline_hash) {
    if (!pipeline_hash)
        pipeline_hash = Pso::HashBytes(source.data(), source.size());
    try {
        DxcBuffer buffer{source.data(), source.size(), CP_UTF8};
        std::vector<LPCWSTR> arguments{L"-E",  entry,   L"-T",   target,
                                       L"-HV", L"2021", L"-Ges", L"-O3"};
        constexpr LPCWSTR optimization_flags[]{L"-O0", L"-O1", L"-O2", L"-O3"};
        arguments.back() = optimization_flags[DxcOptimization()];
        if (DxcStrictEnabled())
            arguments.push_back(L"-Gis");
        ComPtr<DxcOperationResult> result;
        Check(impl->compiler->Compile(
                  &buffer, arguments.data(), static_cast<UINT32>(arguments.size()), nullptr,
                  ResultInterface, reinterpret_cast<void**>(result.GetAddressOf())),
              "DXC Compile");
        HRESULT status{};
        Check(result->GetStatus(&status), "DXC GetStatus");
        ComPtr<DxcBlobEncoding> errors;
        Check(result->GetErrorBuffer(&errors), "DXC GetErrorBuffer");
        if (errors && errors->GetBufferSize()) {
            const std::string_view text{static_cast<const char*>(errors->GetBufferPointer()),
                                        errors->GetBufferSize()};
            LOG_WARNING(Render, "DXC: {}", text);
            if (FAILED(status)) {
                size_t end{};
                for (size_t lines = 0; end < text.size() && end < 4096 && lines < 8; ++end)
                    lines += text[end] == '\n';
                Diagnostic(fmt::format("D3D12 DXC pipeline={:016x} HRESULT={:#010x} errors={}",
                                       pipeline_hash, static_cast<u32>(status),
                                       text.substr(0, end)));
            }
        }
        Check(status, "DXC shader compilation/validation");
        ComPtr<DxcBlob> blob;
        Check(result->GetResult(&blob), "DXC GetResult");
        if (!blob || !blob->GetBufferSize())
            throw std::runtime_error("D3D12 DXC returned empty DXIL");
        const auto* begin = static_cast<const u8*>(blob->GetBufferPointer());
        return {begin, begin + blob->GetBufferSize()};
    } catch (const std::exception& error) {
        Diagnostic(fmt::format("D3D12 DXC pipeline={:016x} what={}", pipeline_hash, error.what()));
        throw;
    }
}
} // namespace D3D12
