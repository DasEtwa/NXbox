// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <tlhelp32.h>
#include <windows.h>

#include "common/logging.h"
#include "common/thread.h"
#include "eden_uwp/thread_profile.h"

// Desktop counterpart of the Xbox sampler; owns its worker through CLI shutdown.
class DesktopThreadProfile {
public:
    ~DesktopThreadProfile() {
        if (worker.joinable()) {
            worker.request_stop();
            wake.notify_all();
            worker.join();
        }
    }
    void Start() {
        if (!EdenXbox::ThreadProfile::Enabled() || worker.joinable())
            return;
        worker = std::jthread{[this](std::stop_token stop) {
            Common::SetCurrentThreadName("ThreadProfiler");
            std::unordered_map<DWORD, EdenXbox::ThreadProfile::CpuSample> samples;
            auto last = GetTickCount64();
            while (!stop.stop_requested()) {
                try {
                    Sample(samples, last);
                } catch (const std::exception& error) {
                    LOG_ERROR(Render, "THREAD_PROFILE sampling failed: {}", error.what());
                } catch (...) {
                    LOG_ERROR(Render, "THREAD_PROFILE sampling failed: unknown exception");
                }
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, std::chrono::seconds{5}, [] { return false; });
            }
        }};
    }

private:
    struct Handle {
        HANDLE value{};
        ~Handle() {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    };
    static std::uint64_t Ticks(FILETIME time) {
        return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    }
    static void Sample(std::unordered_map<DWORD, EdenXbox::ThreadProfile::CpuSample>& samples,
                       ULONGLONG& last) {
        const auto now = GetTickCount64();
        const auto interval = now - last;
        last = now;
        Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)};
        if (snapshot.value == INVALID_HANDLE_VALUE) {
            LOG_ERROR(Render, "THREAD_PROFILE enumeration failed: {}", GetLastError());
            return;
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        std::vector<DWORD> live;
        std::size_t inaccessible{};
        if (Thread32First(snapshot.value, &entry)) {
            do {
                if (entry.th32OwnerProcessID != GetCurrentProcessId())
                    continue;
                Handle thread{
                    OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)};
                FILETIME creation{}, exit{}, kernel{}, user{};
                if (!thread.value ||
                    !GetThreadTimes(thread.value, &creation, &exit, &kernel, &user)) {
                    ++inaccessible;
                    continue;
                }
                live.push_back(entry.th32ThreadID);
                auto& sample = samples[entry.th32ThreadID];
                const bool baseline = !sample.valid || sample.creation != Ticks(creation);
                const auto percent =
                    sample.Update(Ticks(creation), Ticks(kernel) + Ticks(user), now);
                std::string name = "unnamed";
                PWSTR description{};
                if (SUCCEEDED(GetThreadDescription(thread.value, &description))) {
                    const auto cleanup =
                        std::unique_ptr<wchar_t, decltype(&LocalFree)>(description, LocalFree);
                    if (description && *description) {
                        const auto length = WideCharToMultiByte(CP_UTF8, 0, description, -1,
                                                                nullptr, 0, nullptr, nullptr);
                        if (length > 1) {
                            name.resize(static_cast<std::size_t>(length));
                            WideCharToMultiByte(CP_UTF8, 0, description, -1, name.data(), length,
                                                nullptr, nullptr);
                            name.pop_back();
                        }
                    }
                }
                LOG_INFO(Render,
                         "THREAD_PROFILE tid={} name={} cpu_percent={:.2f} baseline={} "
                         "interval_ms={}",
                         entry.th32ThreadID, name, percent, baseline ? 1 : 0, interval);
            } while (Thread32Next(snapshot.value, &entry));
        }
        const auto error = GetLastError();
        std::erase_if(samples, [&](const auto& item) {
            return std::find(live.begin(), live.end(), item.first) == live.end();
        });
        LOG_INFO(Render,
                 "THREAD_PROFILE coverage=process complete={} enumeration_error={} "
                 "inaccessible={} sampled={}",
                 error == ERROR_NO_MORE_FILES ? 1 : 0, error == ERROR_NO_MORE_FILES ? 0 : error,
                 inaccessible, live.size());
        constexpr const char* names[]{"present", "fence"};
        for (std::size_t i = 0; i < EdenXbox::ThreadProfile::waits.size(); ++i) {
            auto& counter = EdenXbox::ThreadProfile::waits[i];
            LOG_INFO(Render, "THREAD_PROFILE host_wait={} total_us={} calls={} interval_ms={}",
                     names[i], counter.microseconds.exchange(0), counter.calls.exchange(0),
                     interval);
        }
    }
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread worker;
};
