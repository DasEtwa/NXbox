// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_disk_pipeline_cache.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <dxgi1_4.h>
#include "video_core/renderer_d3d12/d3d12_context.h"
#endif

namespace D3D12 {
namespace {
constexpr std::array<std::uint8_t, 8> Magic{'N', 'X', 'D', '3', 'D', '1', '2', 'C'};
constexpr std::uint32_t FormatVersion = 1;
constexpr std::size_t HeaderSize = 84;
std::uint64_t PayloadLimit(DiskRecordKind kind) {
    switch (kind) {
    case DiskRecordKind::Dxil:
        return 64ULL << 20;
    case DiskRecordKind::Graphics:
    case DiskRecordKind::Compute:
        return 4ULL << 20;
    case DiskRecordKind::Library:
        return 256ULL << 20;
    }
    return 0;
}
void Put(DiskBytes& bytes, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
std::uint64_t Get(std::span<const std::uint8_t> bytes, std::size_t& offset, unsigned size) {
    std::uint64_t value{};
    for (unsigned i = 0; i < size; ++i)
        value |= std::uint64_t{bytes[offset++]} << (i * 8);
    return value;
}
std::uint64_t Checksum(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes)
        hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
}
std::string Hex(std::uint64_t key) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << key;
    return out.str();
}
std::string Filename(DiskRecordKind kind, std::uint64_t key) {
    switch (kind) {
    case DiskRecordKind::Dxil:
        return Hex(key) + ".dxil";
    case DiskRecordKind::Graphics:
        return Hex(key) + ".graphics";
    case DiskRecordKind::Compute:
        return Hex(key) + ".compute";
    case DiskRecordKind::Library:
        return "pso.library";
    }
    return {};
}
} // namespace

DiskBytes SerializeDiskRecord(const DiskCacheIdentity& identity, const DiskPipelineRecord& record) {
    if (record.payload.empty() || record.payload.size() > PayloadLimit(record.kind) ||
        (record.kind == DiskRecordKind::Library && record.key != 0)) {
        throw std::invalid_argument("Invalid D3D12 disk record");
    }
    DiskBytes bytes(Magic.begin(), Magic.end());
    Put(bytes, FormatVersion, 4);
    for (const auto value :
         {identity.emitter_version, identity.dxc_version, identity.schema_version,
          identity.vendor_id, identity.device_id, identity.subsystem_id, identity.revision})
        Put(bytes, value, 4);
    Put(bytes, identity.adapter_luid, 8);
    Put(bytes, identity.driver_version, 8);
    Put(bytes, static_cast<std::uint32_t>(record.kind), 4);
    Put(bytes, record.key, 8);
    Put(bytes, record.payload.size(), 8);
    bytes.insert(bytes.end(), record.payload.begin(), record.payload.end());
    Put(bytes, Checksum(bytes), 8);
    return bytes;
}

std::optional<DiskPipelineRecord> DeserializeDiskRecord(std::span<const std::uint8_t> bytes,
                                                        const DiskCacheIdentity& identity) {
    if (bytes.size() < HeaderSize || !std::equal(Magic.begin(), Magic.end(), bytes.begin()))
        return {};
    std::size_t offset = Magic.size();
    if (Get(bytes, offset, 4) != FormatVersion)
        return {};
    for (const auto expected :
         {identity.emitter_version, identity.dxc_version, identity.schema_version,
          identity.vendor_id, identity.device_id, identity.subsystem_id, identity.revision}) {
        if (Get(bytes, offset, 4) != expected)
            return {};
    }
    if (Get(bytes, offset, 8) != identity.adapter_luid ||
        Get(bytes, offset, 8) != identity.driver_version)
        return {};
    DiskPipelineRecord record;
    record.kind = static_cast<DiskRecordKind>(Get(bytes, offset, 4));
    record.key = Get(bytes, offset, 8);
    const auto size = Get(bytes, offset, 8);
    if (!size || size > PayloadLimit(record.kind) || size != bytes.size() - HeaderSize ||
        (record.kind == DiskRecordKind::Library && record.key != 0))
        return {};
    std::size_t checksum_offset = bytes.size() - 8;
    if (Get(bytes, checksum_offset, 8) != Checksum(bytes.first(bytes.size() - 8)))
        return {};
    record.payload.assign(bytes.begin() + offset, bytes.end() - 8);
    return record;
}

struct DiskPipelineCache::Impl {
    std::filesystem::path directory;
    DiskCacheIdentity identity;
    std::mutex mutex;
    std::jthread worker;
#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    // CreatePipelineLibrary retains this pointer. Destroy the library first.
    DiskBytes library_input;
    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> library;
    bool library_dirty{};
#endif
    std::optional<DiskPipelineRecord> Read(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return {};
        const auto size = file.tellg();
        const auto extension = path.extension();
        const auto limit = extension == ".dxil"      ? PayloadLimit(DiskRecordKind::Dxil)
                           : extension == ".library" ? PayloadLimit(DiskRecordKind::Library)
                                                     : PayloadLimit(DiskRecordKind::Graphics);
        if (size < static_cast<std::streamoff>(HeaderSize) ||
            size > static_cast<std::streamoff>(HeaderSize + limit)) {
            file.close();
            std::error_code error;
            std::filesystem::remove(path, error);
            return {};
        }
        DiskBytes bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        auto record = file ? DeserializeDiskRecord(bytes, identity) : std::nullopt;
        file.close();
        if (record && path.filename() == Filename(record->kind, record->key))
            return record;
        std::error_code error;
        std::filesystem::remove(path, error);
        return {};
    }
    bool Write(const DiskPipelineRecord& record) {
        if (directory.empty())
            return false;
        try {
            const auto bytes = SerializeDiskRecord(identity, record);
            const auto path = directory / Filename(record.kind, record.key);
            auto temp = path;
            temp += ".tmp";
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            file.flush();
            const bool good = static_cast<bool>(file);
            file.close();
            bool replaced = false;
            if (good && file) {
                std::error_code error;
                std::filesystem::rename(temp, path, error);
                replaced = !error;
            }
            if (!replaced) {
                std::error_code error;
                std::filesystem::remove(temp, error);
            }
            return replaced;
        } catch (const std::exception&) {
            return false;
        }
    }
};

DiskPipelineCache::DiskPipelineCache(const std::filesystem::path& local_state,
                                     std::uint64_t title_id, DiskCacheIdentity identity)
    : impl{std::make_unique<Impl>()} {
    impl->identity = identity;
    if (!title_id || local_state.empty())
        return;
    const auto directory = local_state / "d3d12_cache" / Hex(title_id);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (!error)
        impl->directory = directory;
}
DiskPipelineCache::~DiskPipelineCache() {
    StopWarmup();
    try {
        Flush();
    } catch (...) {
        // Optional cache persistence must not throw during teardown.
    }
}
std::optional<DiskBytes> DiskPipelineCache::FindDxil(std::uint64_t hlsl_hash) {
    std::scoped_lock lock{impl->mutex};
    if (impl->directory.empty())
        return {};
    auto record = impl->Read(impl->directory / Filename(DiskRecordKind::Dxil, hlsl_hash));
    if (!record)
        return {};
    return std::move(record->payload);
}
bool DiskPipelineCache::StoreDxil(std::uint64_t hlsl_hash, std::span<const std::uint8_t> dxil) {
    std::scoped_lock lock{impl->mutex};
    return impl->Write({DiskRecordKind::Dxil, hlsl_hash, DiskBytes(dxil.begin(), dxil.end())});
}
bool DiskPipelineCache::RememberPipeline(const DiskPipelineRecord& record) {
    if (record.kind != DiskRecordKind::Graphics && record.kind != DiskRecordKind::Compute)
        return false;
    std::scoped_lock lock{impl->mutex};
    return impl->Write(record);
}
void DiskPipelineCache::WaitWarmup() {
    if (impl->worker.joinable())
        impl->worker.join();
}
void DiskPipelineCache::StopWarmup() {
    if (impl->worker.joinable()) {
        impl->worker.request_stop();
        impl->worker.join();
    }
}
void DiskPipelineCache::StartWarmup(BuildCallback build, ProgressCallback progress,
                                    std::stop_token stop_loading) {
    StopWarmup();
    impl->worker = std::jthread([this, build = std::move(build), progress = std::move(progress),
                                 stop_loading](std::stop_token stop) {
        std::stop_source combined_stop;
        std::stop_callback forward_owner(stop, [&] { combined_stop.request_stop(); });
        std::stop_callback forward_loading(stop_loading, [&] { combined_stop.request_stop(); });
        const auto token = combined_stop.get_token();
        const auto cancelled = [&] { return token.stop_requested(); };
        DiskWarmupProgress state{DiskWarmupStage::Begin};
        const auto notify = [&] {
            if (progress) {
                try {
                    progress(state);
                } catch (...) {
                }
            }
        };
        try {
            std::vector<std::filesystem::path> paths;
            if (!impl->directory.empty()) {
                std::error_code error;
                std::filesystem::directory_iterator it(impl->directory, error), end;
                while (!error && it != end && !cancelled()) {
                    const auto extension = it->path().extension();
                    if (extension == ".graphics" || extension == ".compute")
                        paths.push_back(it->path());
                    it.increment(error);
                }
            }
            std::sort(paths.begin(), paths.end());
            state.total = paths.size();
            notify();
            for (const auto& path : paths) {
                if (cancelled())
                    break;
                bool built = false;
                try {
                    std::optional<DiskPipelineRecord> record;
                    {
                        std::scoped_lock lock{impl->mutex};
                        record = impl->Read(path);
                    }
                    if (record && build)
                        built = build(*record, token);
                } catch (...) {
                }
                ++state.completed;
                if (!built)
                    ++state.failed;
                state.stage = DiskWarmupStage::Build;
                notify();
            }
            const bool flushed = Flush();
            state.stage = cancelled() ? DiskWarmupStage::Cancelled
                          : flushed   ? DiskWarmupStage::Complete
                                      : DiskWarmupStage::Failed;
        } catch (...) {
            state.stage = DiskWarmupStage::Failed;
        }
        notify();
    });
}

#ifdef _WIN32
namespace {
bool CacheSucceeded(HRESULT result, const char* operation, ID3D12Device* device,
                    std::uint64_t key = 0) {
    if (SUCCEEDED(result))
        return true;
    try {
        if (key)
            Diagnostic("D3D12 optional cache pipeline=" + Hex(key));
        Check(result, operation, device);
    } catch (const std::exception&) {
        // A cache miss or unsupported library is recoverable, but keep its HRESULT.
    }
    if (device)
        Check(device->GetDeviceRemovedReason(), "optional cache device removed", device);
    return false;
}
} // namespace
std::optional<DiskCacheIdentity> DiskPipelineCache::QueryIdentity(ID3D12Device* device,
                                                                  DiskCacheIdentity versions) {
    if (!device)
        return {};
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto luid = device->GetAdapterLuid();
    DXGI_ADAPTER_DESC1 desc{};
    LARGE_INTEGER driver{};
    if (!CacheSucceeded(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "cache CreateDXGIFactory1",
                        device) ||
        !CacheSucceeded(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)),
                        "cache EnumAdapterByLuid", device) ||
        !CacheSucceeded(adapter->GetDesc1(&desc), "cache GetDesc1", device) ||
        !CacheSucceeded(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver),
                        "cache driver identity", device))
        return {};
    versions.vendor_id = desc.VendorId;
    versions.device_id = desc.DeviceId;
    versions.subsystem_id = desc.SubSysId;
    versions.revision = desc.Revision;
    versions.adapter_luid =
        (std::uint64_t{static_cast<std::uint32_t>(luid.HighPart)} << 32) | luid.LowPart;
    versions.driver_version = static_cast<std::uint64_t>(driver.QuadPart);
    return versions;
}
DiskPipelineCache::DiskPipelineCache(const std::filesystem::path& local_state,
                                     std::uint64_t title_id, DiskCacheIdentity identity,
                                     ID3D12Device* device)
    : DiskPipelineCache(local_state, title_id, identity) {
    impl->device = device;
    Check(device->GetDeviceRemovedReason(), "before pipeline library identity", device);
    const auto actual = QueryIdentity(device, identity);
    if (!actual) {
        impl->directory.clear();
        return;
    }
    impl->identity = *actual;
    Diagnostic("D3D12 cache identity luid=" + Hex(actual->adapter_luid) + " driver=" +
               Hex(actual->driver_version) + " emitter=" + std::to_string(actual->emitter_version) +
               " dxc=" + std::to_string(actual->dxc_version) +
               " schema=" + std::to_string(actual->schema_version));
    if (BisectEnabled("NXBOX_D3D12_NO_PSO_LIBRARY")) {
        Diagnostic("D3D12 pipeline library disabled; direct PSO creation");
        return;
    }
#ifdef NXBOX_UWP
    if (!BisectEnabled("NXBOX_D3D12_PSO_LIBRARY")) {
        Diagnostic("D3D12 pipeline library disabled on Xbox/UWP; DXIL disk cache active");
        return;
    }
#endif
    Microsoft::WRL::ComPtr<ID3D12Device1> device1;
    D3D12_FEATURE_DATA_SHADER_CACHE support{};
    if (!CacheSucceeded(
            device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support)),
            "cache shader support", device) ||
        !(support.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) ||
        !CacheSucceeded(device->QueryInterface(IID_PPV_ARGS(&device1)), "cache ID3D12Device1",
                        device))
        return;
    if (!impl->directory.empty()) {
        const auto path = impl->directory / "pso.library";
        std::error_code error;
        const bool existed = std::filesystem::exists(path, error);
        auto record = impl->Read(path);
        if (existed && !record)
            Diagnostic("D3D12 pipeline library identity/version/checksum mismatch; blob deleted");
        if (record)
            impl->library_input = std::move(record->payload);
    }
    Check(device->GetDeviceRemovedReason(), "before CreatePipelineLibrary", device);
    Diagnostic("D3D12 CreatePipelineLibrary begin bytes=" +
               std::to_string(impl->library_input.size()));
    auto result = device1->CreatePipelineLibrary(
        impl->library_input.empty() ? nullptr : impl->library_input.data(),
        impl->library_input.size(), IID_PPV_ARGS(&impl->library));
    if (!CacheSucceeded(result,
                        impl->library_input.empty() ? "CreatePipelineLibrary empty initial"
                                                    : "CreatePipelineLibrary from disk",
                        device)) {
        impl->library.Reset();
        impl->library_input.clear();
        if (!impl->directory.empty()) {
            std::error_code error;
            std::filesystem::remove(impl->directory / "pso.library", error);
        }
        CacheSucceeded(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&impl->library)),
                       "CreatePipelineLibrary empty", device);
    }
    Check(device->GetDeviceRemovedReason(), "after CreatePipelineLibrary", device);
    Diagnostic("D3D12 CreatePipelineLibrary complete");
}
Microsoft::WRL::ComPtr<ID3D12PipelineState> DiskPipelineCache::LoadOrCreateGraphics(
    std::uint64_t key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
    std::span<const std::uint8_t> recipe, FrameProfile* profile) {
    std::scoped_lock lock{impl->mutex};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (!impl->device)
        return pso;
    const auto hex = Hex(key);
    const auto name = L"g" + std::wstring(hex.begin(), hex.end());
    if (impl->library &&
        CacheSucceeded(impl->library->LoadGraphicsPipeline(name.c_str(), &desc, IID_PPV_ARGS(&pso)),
                       "LoadGraphicsPipeline (optional cache)", impl->device.Get(), key)) {
        impl->Write({DiskRecordKind::Graphics, key, DiskBytes(recipe.begin(), recipe.end())});
        return pso;
    }
    if (profile)
        profile->Count(FrameCount::PsoCompiles);
    CheckPipeline(impl->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)),
                  "CreateGraphicsPipelineState", impl->device.Get(), key);
    if (impl->library && CacheSucceeded(impl->library->StorePipeline(name.c_str(), pso.Get()),
                                        "StorePipeline", impl->device.Get(), key))
        impl->library_dirty = true;
    impl->Write({DiskRecordKind::Graphics, key, DiskBytes(recipe.begin(), recipe.end())});
    return pso;
}
Microsoft::WRL::ComPtr<ID3D12PipelineState> DiskPipelineCache::LoadOrCreateCompute(
    std::uint64_t key, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
    std::span<const std::uint8_t> recipe, FrameProfile* profile) {
    std::scoped_lock lock{impl->mutex};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (!impl->device)
        return pso;
    const auto hex = Hex(key);
    const auto name = L"c" + std::wstring(hex.begin(), hex.end());
    if (impl->library &&
        CacheSucceeded(impl->library->LoadComputePipeline(name.c_str(), &desc, IID_PPV_ARGS(&pso)),
                       "LoadComputePipeline (optional cache)", impl->device.Get(), key)) {
        impl->Write({DiskRecordKind::Compute, key, DiskBytes(recipe.begin(), recipe.end())});
        return pso;
    }
    if (profile)
        profile->Count(FrameCount::PsoCompiles);
    CheckPipeline(impl->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)),
                  "CreateComputePipelineState", impl->device.Get(), key);
    if (impl->library && CacheSucceeded(impl->library->StorePipeline(name.c_str(), pso.Get()),
                                        "StorePipeline", impl->device.Get(), key))
        impl->library_dirty = true;
    impl->Write({DiskRecordKind::Compute, key, DiskBytes(recipe.begin(), recipe.end())});
    return pso;
}
#endif
bool DiskPipelineCache::Flush() {
    std::scoped_lock lock{impl->mutex};
#ifdef _WIN32
    if (impl->library && impl->library_dirty) {
        const auto size = impl->library->GetSerializedSize();
        if (!size || size > PayloadLimit(DiskRecordKind::Library))
            return false;
        DiskBytes bytes(size);
        if (!CacheSucceeded(impl->library->Serialize(bytes.data(), bytes.size()),
                            "SerializePipelineLibrary", impl->device.Get()) ||
            !impl->Write({DiskRecordKind::Library, 0, std::move(bytes)}))
            return false;
        impl->library_dirty = false;
    }
#endif
    return true;
}
} // namespace D3D12
