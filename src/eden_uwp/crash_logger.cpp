// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/crash_logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include <windows.h>
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/thread_profile.h"

#include <fileapifromapp.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.System.h>

#include "common/nxbox_stall.h"
#include "eden_uwp/crash_format.h"

namespace EdenXbox {
namespace {
std::atomic<HANDLE> crash_file{INVALID_HANDLE_VALUE};
DWORD main_thread_id{};
thread_local bool in_crash_handler{};
static_assert(std::atomic<HANDLE>::is_always_lock_free);

const char* ThreadRole(DWORD id) noexcept {
    if (id == NxboxStall::gpu_thread_id.load(std::memory_order_relaxed)) {
        return "gpu";
    }
    constexpr const char* names[]{"cpu0", "cpu1", "cpu2", "cpu3"};
    for (unsigned core = 0; core < 4; ++core) {
        if (id == NxboxStall::cpu_thread_ids[core].load(std::memory_order_relaxed)) {
            return names[core];
        }
    }
    return id == main_thread_id ? "main" : "worker";
}

void Prefix(Crash::Line& line, std::string_view event) noexcept {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    line.Add("utc_100ns=");
    line.Number((std::uint64_t{now.dwHighDateTime} << 32) | now.dwLowDateTime);
    line.Add(" tick_ms=");
    line.Number(GetTickCount64());
    line.Add(" pid=");
    line.Number(GetCurrentProcessId());
    line.Add(" tid=");
    const auto id = GetCurrentThreadId();
    line.Number(id);
    line.Add(" role=");
    line.Add(ThreadRole(id));
    line.Add(" event=");
    line.Add(event);
}

void Write(Crash::Line& line) noexcept {
    line.Finish();
    const auto file = crash_file.load(std::memory_order_acquire);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        // GENERIC_WRITE permits FlushFileBuffers; sentinel offsets append without seek/locks.
        OVERLAPPED append{};
        append.Offset = append.OffsetHigh = MAXDWORD;
        WriteFile(file, line.data, static_cast<DWORD>(line.size), &written, &append);
        FlushFileBuffers(file);
    } else {
        OutputDebugStringA(line.data);
    }
}

void ResolveAddress(Crash::Line& line, const void* address) noexcept {
    HMODULE module{};
    char name[256]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        const auto length = GetModuleFileNameA(module, name, sizeof(name));
        if (!length || length >= sizeof(name)) {
            name[0] = '\0';
        }
    }
    line.Address(name, reinterpret_cast<std::uintptr_t>(address),
                 reinterpret_cast<std::uintptr_t>(module));
}

void Address(Crash::Line& line, const void* address) noexcept {
    __try {
        ResolveAddress(line, address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        line.Add("unavailable+0x");
        line.Number(reinterpret_cast<std::uintptr_t>(address), 16);
    }
}

USHORT Capture(void** frames) noexcept {
    __try {
        return RtlCaptureStackBackTrace(0, 96, frames, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void RecordCrash(const char* event, DWORD code, EXCEPTION_POINTERS* info = nullptr) noexcept {
    if (in_crash_handler) {
        return;
    }
    in_crash_handler = true;
    // Persist the reason BEFORE any loader/stack work, which can itself fault or deadlock.
    {
        Crash::Line line;
        Prefix(line, event);
        line.Add(" code=0x");
        line.Number(code, 16);
        if (info) {
            line.Add(" pc=0x");
            line.Number(reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress),
                        16);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                info->ExceptionRecord->NumberParameters >= 2) {
                line.Add(" access=");
                line.Number(info->ExceptionRecord->ExceptionInformation[0]);
                line.Add(" target=0x");
                line.Number(info->ExceptionRecord->ExceptionInformation[1], 16);
            }
        }
        Write(line);
    }
    if (info) {
        Crash::Line line;
        Prefix(line, "fault");
        line.Add(" address=");
        Address(line, info->ExceptionRecord->ExceptionAddress);
        Write(line);
    }
    // This is the handler/caller stack, not a reconstruction of the interrupted stack. The
    // exception PC above is authoritative. Use the UWP stack capture without DbgHelp symbols.
    void* frames[96]{};
    const auto count = Capture(frames);
    if (!count) {
        CrashEvent("stack_unavailable");
    }
    for (unsigned index = 0; index < count; ++index) {
        Crash::Line line;
        Prefix(line, "handler_stack");
        line.Add(" frame=");
        line.Number(index);
        line.Add(" address=");
        Address(line, frames[index]);
        Write(line);
    }
    in_crash_handler = false;
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* info) noexcept {
    RecordCrash("unhandled", info->ExceptionRecord->ExceptionCode, info);
    return EXCEPTION_CONTINUE_SEARCH;
}

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
// These APIs are excluded by Microsoft's UWP SDK partition. Never import/resolve them in APP.
LONG WINAPI Vectored(EXCEPTION_POINTERS* info) noexcept {
    if (Crash::FatalCode(info->ExceptionRecord->ExceptionCode, false)) {
        RecordCrash("first_chance", info->ExceptionRecord->ExceptionCode, info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

[[noreturn]] void FatalRuntime(const char* event, DWORD code) noexcept {
    // A SIGABRT from C++ unwinding previously hid the exception and truncated the
    // caller stack before it reached guest rendering code.
    if (const auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            Crash::Line line;
            Prefix(line, "active_exception");
            line.Add(" what=");
            line.Add(error.what());
            Write(line);
        } catch (...) {
            CrashEvent("active_exception_unknown");
        }
    }
    RecordCrash(event, code);
    RaiseFailFastException(nullptr, nullptr, 0);
    std::_Exit(3);
}
void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {
    FatalRuntime("invalid_parameter", 0xc0000417);
}
void MemoryEvent(const char* event, std::uint64_t next_limit = 0) noexcept {
    try {
        using winrt::Windows::System::MemoryManager;
        CrashEvent(event, MemoryManager::AppMemoryUsage(), MemoryManager::AppMemoryUsageLimit(),
                   next_limit);
    } catch (...) {
        CrashEvent("memory_query_failed");
    }
}

// One heartbeat/stall line: memory, usage level, D3D12 device-removed flag (published by Mesa as
// an environment variable) and the age of the last presented game frame.
void HeartbeatLine(const char* event, std::uint64_t gap_ms) noexcept {
    try {
        using winrt::Windows::System::MemoryManager;
        Crash::Line line;
        Prefix(line, event);
        line.Add(" usage=");
        line.Number(MemoryManager::AppMemoryUsage());
        line.Add(" limit=");
        line.Number(MemoryManager::AppMemoryUsageLimit());
        line.Add(" level=");
        line.Number(static_cast<std::uint32_t>(MemoryManager::AppMemoryUsageLevel()));
        char lost[2]{};
        line.Add(" device_lost=");
        line.Add(GetEnvironmentVariableA("NXBOX_D3D12_DEVICE_LOST", lost, sizeof(lost)) == 1 &&
                         lost[0] == '1'
                     ? "1"
                     : "0");
        const auto present = NxboxStall::last_present_tick_ms.load(std::memory_order_relaxed);
        line.Add(" present_age_ms=");
        line.Number(present ? GetTickCount64() - present : 0);
        if (gap_ms) {
            line.Add(" gap_ms=");
            line.Number(gap_ms);
        }
        Write(line);
    } catch (...) {
        CrashEvent("memory_query_failed");
    }
}
struct ProfileHandle {
    HANDLE value{};
    explicit ProfileHandle(HANDLE value_) noexcept : value{value_} {}
    ~ProfileHandle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    ProfileHandle(const ProfileHandle&) = delete;
    ProfileHandle& operator=(const ProfileHandle&) = delete;
};
std::mutex profile_mutex;
std::vector<DWORD> registered_threads;

std::uint64_t FileTicks(FILETIME time) noexcept {
    return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
}

// Toolhelp is optional on Xbox. Resolve it dynamically so profiling cannot add a
// desktop-only loader dependency to the UWP executable. This is THREADENTRY32's ABI.
struct ProfileThreadEntry {
    DWORD size;
    DWORD usage;
    DWORD id;
    DWORD process;
    LONG base_priority;
    LONG delta_priority;
    DWORD flags;
};
static_assert(sizeof(ProfileThreadEntry) == 28);

void SampleThreads() noexcept {
    try {
        static std::mutex sampling_mutex;
        const std::unique_lock sampling_lock{sampling_mutex, std::try_to_lock};
        if (!sampling_lock.owns_lock())
            return;
        static std::unordered_map<DWORD, ThreadProfile::CpuSample> samples;
        static auto last = GetTickCount64();
        const auto now = GetTickCount64();
        const auto interval = now - last;
        last = now;
        std::vector<DWORD> ids;
        {
            const std::lock_guard lock{profile_mutex};
            ids = registered_threads;
        }
        const auto kernel = GetModuleHandleW(L"kernel32.dll");
        using Snapshot = HANDLE(WINAPI*)(DWORD, DWORD);
        using ThreadEntry = BOOL(WINAPI*)(HANDLE, ProfileThreadEntry*);
        const auto snapshot =
            reinterpret_cast<Snapshot>(GetProcAddress(kernel, "CreateToolhelp32Snapshot"));
        const auto first = reinterpret_cast<ThreadEntry>(GetProcAddress(kernel, "Thread32First"));
        const auto next = reinterpret_cast<ThreadEntry>(GetProcAddress(kernel, "Thread32Next"));
        bool complete = false;
        DWORD enumeration_error = ERROR_NOT_SUPPORTED;
        if (snapshot && first && next) {
            ProfileHandle handle{snapshot(4 /* TH32CS_SNAPTHREAD */, 0)};
            if (handle.value != INVALID_HANDLE_VALUE) {
                ProfileThreadEntry entry{};
                entry.size = sizeof(entry);
                if (first(handle.value, &entry)) {
                    do {
                        if (entry.process == GetCurrentProcessId())
                            ids.push_back(entry.id);
                        entry.size = sizeof(entry);
                    } while (next(handle.value, &entry));
                    enumeration_error = GetLastError();
                    complete = enumeration_error == ERROR_NO_MORE_FILES;
                } else {
                    enumeration_error = GetLastError();
                }
            } else {
                enumeration_error = GetLastError();
            }
        }
        ids.push_back(main_thread_id);
        ids.push_back(NxboxStall::gpu_thread_id.load());
        for (const auto& id : NxboxStall::cpu_thread_ids)
            ids.push_back(id.load());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        std::vector<DWORD> live;
        std::size_t inaccessible = 0;
        for (const auto id : ids) {
            if (!id)
                continue;
            ProfileHandle thread{OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id)};
            if (!thread.value) {
                ++inaccessible;
                continue;
            }
            FILETIME creation{}, exit{}, kernel_time{}, user_time{};
            if (!GetThreadTimes(thread.value, &creation, &exit, &kernel_time, &user_time)) {
                ++inaccessible;
                continue;
            }
            if (FileTicks(exit))
                continue;
            live.push_back(id);
            auto& sample = samples[id];
            const bool baseline = !sample.valid || sample.creation != FileTicks(creation);
            const double percent = sample.Update(
                FileTicks(creation), FileTicks(kernel_time) + FileTicks(user_time), now);
            std::string name = ThreadRole(id);
            PWSTR description{};
            if (SUCCEEDED(GetThreadDescription(thread.value, &description))) {
                // LocalFree even if conversion or formatting throws.
                const auto cleanup =
                    std::unique_ptr<wchar_t, decltype(&LocalFree)>(description, LocalFree);
                if (description && *description) {
                    const auto length = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0,
                                                            nullptr, nullptr);
                    if (length > 1) {
                        std::string utf8(static_cast<std::size_t>(length), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, description, -1, utf8.data(), length,
                                            nullptr, nullptr);
                        utf8.pop_back();
                        name = std::move(utf8);
                    }
                }
            }
            Diagnostic(fmt::format(
                "THREAD_PROFILE tid={} name={} cpu_percent={:.2f} baseline={} interval_ms={}", id,
                name, percent, baseline ? 1 : 0, interval));
        }
        std::erase_if(samples, [&](const auto& item) {
            return std::find(live.begin(), live.end(), item.first) == live.end();
        });
        {
            const std::lock_guard lock{profile_mutex};
            // Remove terminated threads; future registrations restore newly reused IDs.
            std::erase_if(registered_threads, [&](DWORD id) {
                return std::find(live.begin(), live.end(), id) == live.end();
            });
        }
        Diagnostic(fmt::format("THREAD_PROFILE coverage=process complete={} enumeration_error={} "
                               "inaccessible={} sampled={}",
                               complete ? 1 : 0, complete ? 0 : enumeration_error, inaccessible,
                               live.size()));
        constexpr const char* names[]{"present", "fence"};
        for (std::size_t index = 0; index < ThreadProfile::waits.size(); ++index) {
            auto& counter = ThreadProfile::waits[index];
            const auto us = counter.microseconds.exchange(0, std::memory_order_relaxed);
            const auto calls = counter.calls.exchange(0, std::memory_order_relaxed);
            Diagnostic(
                fmt::format("THREAD_PROFILE host_wait={} total_us={} calls={} interval_ms={}",
                            names[index], us, calls, interval));
        }
    } catch (const std::exception& error) {
        CrashEvent(error.what());
    } catch (...) {
        CrashEvent("thread_profile_non_standard_exception");
    }
}

} // namespace

void RegisterProfileThread() noexcept {
    if (!ThreadProfile::Enabled())
        return;
    try {
        const std::lock_guard lock{profile_mutex};
        const auto id = GetCurrentThreadId();
        if (std::find(registered_threads.begin(), registered_threads.end(), id) ==
            registered_threads.end())
            registered_threads.push_back(id);
    } catch (...) {
        CrashEvent("thread_profile_registration_failed");
    }
}

void StartThreadProfiler() noexcept {
    if (!ThreadProfile::Enabled())
        return;
    try {
        RegisterProfileThread();
        static const auto profiler = [] {
            Diagnostic("THREAD_PROFILE enabled period_ms=5000 cpu_percent=one_host_core");
            return winrt::Windows::System::Threading::ThreadPoolTimer::CreatePeriodicTimer(
                [](const auto&) { SampleThreads(); }, std::chrono::seconds(5));
        }();
    } catch (const std::exception& error) {
        CrashRendererException("thread_profile_start", error.what());
    } catch (...) {
        CrashEvent("thread_profile_start_non_standard_exception");
    }
}

void InstallThreadCrashHandlers() noexcept {
    RegisterProfileThread();
    // MSVC maintains terminate handlers per thread, including threads created later.
    std::set_terminate([] { FatalRuntime("terminate", 0); });
}

void InstallCrashHandlers() noexcept {
    main_thread_id = GetCurrentThreadId();
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    AddVectoredExceptionHandler(1, Vectored);
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
#endif
    SetUnhandledExceptionFilter(Unhandled);
    InstallThreadCrashHandlers();
    _set_invalid_parameter_handler(InvalidParameter);
    _set_purecall_handler([] { FatalRuntime("purecall", 0xc0000025); });
    std::signal(SIGABRT, [](int) { FatalRuntime("SIGABRT", SIGABRT); });
}

void OpenCrashLog() {
    // Called immediately after apartment initialization, before CoreApplication::Run or workers.
    const auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    const std::wstring path = std::wstring(folder) + L"\\nxbox_crash.txt";
    const auto file = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
    crash_file.store(file, std::memory_order_release);
    CrashEvent(file == INVALID_HANDLE_VALUE ? "crash_log_open_failed" : "process_start");
    CrashEvent("uwp_no_vectored_handler_or_stack_guarantee");
    // Kept open for the process lifetime, including static destruction/terminate.
}

void CrashEvent(std::string_view event, std::uint64_t usage, std::uint64_t limit,
                std::uint64_t next_limit) noexcept {
    Crash::Line line;
    Prefix(line, event);
    line.Add(" usage=");
    line.Number(usage);
    line.Add(" limit=");
    line.Number(limit);
    line.Add(" next_limit=");
    line.Number(next_limit);
    Write(line);
}

void CrashRendererException(const char* stage, std::string_view reason) noexcept {
    std::uint64_t part = 0;
    do {
        Crash::Line line;
        Prefix(line, "renderer_exception");
        line.Add(" stage=");
        line.Add(stage);
        line.Add(" part=");
        line.Number(part++);
        line.Add(" what=");
        // Chunk long reasons instead of truncating them in the fixed crash buffer.
        const auto count = (std::min)(reason.size(), std::size_t{192});
        line.Add(reason.substr(0, count));
        Write(line);
        reason.remove_prefix(count);
    } while (!reason.empty());
}

void CrashGpuError(const char* stage, std::int32_t result, std::int32_t removed) noexcept {
    Crash::Line line;
    Prefix(line, "gpu_error");
    line.Add(" stage=");
    line.Add(stage);
    line.Add(" hr=0x");
    line.Number(static_cast<std::uint32_t>(result), 16);
    line.Add(" removed=0x");
    line.Number(static_cast<std::uint32_t>(removed), 16);
    Write(line);
}

void ObserveCrashLifecycle() {
    using winrt::Windows::ApplicationModel::Core::CoreApplication;
    using winrt::Windows::System::MemoryManager;
    // Process-lifetime subscriptions, installed before activation/library/game selection.
    CoreApplication::Suspending([](const auto&, const auto&) { MemoryEvent("Suspending"); });
    CoreApplication::Resuming([](const auto&, const auto&) { MemoryEvent("Resuming"); });
    CoreApplication::EnteredBackground(
        [](const auto&, const auto&) { MemoryEvent("EnteredBackground"); });
    CoreApplication::LeavingBackground(
        [](const auto&, const auto&) { MemoryEvent("LeavingBackground"); });
    MemoryManager::AppMemoryUsageIncreased(
        [](const auto&, const auto&) { MemoryEvent("AppMemoryUsageIncreased"); });
    MemoryManager::AppMemoryUsageLimitChanging([](const auto&, const auto& args) {
        MemoryEvent("AppMemoryUsageLimitChanging", args.NewLimit());
    });
    MemoryEvent("memory_initial");
    StartThreadProfiler();
    // Samples every 100 ms while the UI/emulation threads stall; suspension pauses the timer. A
    // line is written once per second, plus a `stall` line the moment a gap over 300 ms ends.
    static std::atomic<std::uint64_t> last_sample_ms{0};
    static std::atomic<std::uint64_t> last_write_ms{0};
    static const auto heartbeat =
        winrt::Windows::System::Threading::ThreadPoolTimer::CreatePeriodicTimer(
            [](const auto&) {
                const auto now = GetTickCount64();
                const auto previous = last_sample_ms.exchange(now);
                if (previous && now - previous > 300) {
                    HeartbeatLine("stall", now - previous);
                }
                auto written = last_write_ms.load();
                if (now - written >= 1000 && last_write_ms.compare_exchange_strong(written, now)) {
                    HeartbeatLine("memory_heartbeat", 0);
                }
            },
            std::chrono::milliseconds(100));
}
} // namespace EdenXbox

// Mesa resolves this exported, allocation-free callback from the executable on a failed API.
extern "C" __declspec(dllexport) void WINAPI NXboxCrashGpuError(const char* stage, HRESULT result,
                                                                HRESULT removed) noexcept {
    EdenXbox::CrashGpuError(stage, result, removed);
}
