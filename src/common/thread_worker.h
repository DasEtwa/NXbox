// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <queue>

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "common/unique_function.h"

#ifdef NXBOX_UWP
#include "eden_uwp/crash_logger.h"
#endif

namespace Common {

template <class StateType = void>
class StatefulThreadWorker {
    static constexpr bool with_state = !std::is_same_v<StateType, void>;

    struct DummyCallable {
        int operator()() const noexcept {
            return 0;
        }
    };

    using Task =
        std::conditional_t<with_state, UniqueFunction<void, StateType*>, UniqueFunction<void>>;
    using StateMaker = std::conditional_t<with_state, std::function<StateType()>, DummyCallable>;

public:
    explicit StatefulThreadWorker(size_t num_workers, std::string name, StateMaker func = {},
                                  ThreadPlacement placement = ThreadPlacement::Default)
        : workers_queued{num_workers}, thread_name{std::move(name)} {
        const auto lambda = [this, func, placement](std::stop_token stop_token) {
            try {
            Common::SetCurrentThreadName(thread_name.c_str());
            if (placement != ThreadPlacement::Default) {
                Common::SetCurrentThreadPriority(ThreadPriority::Low);
            }
            switch (placement) {
            case ThreadPlacement::Efficiency:
                Common::SetCurrentThreadToEfficiencyCores();
                break;
            case ThreadPlacement::Background:
                Common::SetCurrentThreadToBackgroundWork();
                break;
            default:
                Common::SetCurrentThreadToAllCores();
                break;
            }
            } catch (const std::exception& error) {
                ReportException("Worker setup", error.what());
            } catch (...) {
                ReportException("Worker setup", "non-standard exception");
            }
            {
                using WorkerState = std::conditional_t<with_state, StateType, int>;
                std::unique_ptr<WorkerState> state;
                const auto initialize = [&] {
                    // Direct initialization preserves copy elision for thread-bound
                    // OpenGL contexts and states that cannot be moved.
                    state.reset(new WorkerState(func()));
                };
                try {
                    initialize();
                } catch (const std::exception& error) {
                    ReportException("Worker initialization", error.what());
                } catch (...) {
                    ReportException("Worker initialization", "non-standard exception");
                }
                while (!stop_token.stop_requested()) {
                    Task task;
                    {
                        std::unique_lock lock{queue_mutex};
                        if (requests.empty()) {
                            wait_condition.notify_all();
                        }
                        condition.wait(lock, stop_token,
                                       [this] { return !requests.empty(); });
                        if (stop_token.stop_requested()) {
                            break;
                        }
                        task = std::move(requests.front());
                        requests.pop();
                    }
                    try {
                        // Retry failed initialization on demand, without spinning or passing
                        // an invalid state to a task. A failed attempt consumes this task.
                        if (!state) {
                            initialize();
                        }
                        if constexpr (with_state) {
                            task(state.get());
                        } else {
                            task();
                        }
                    } catch (const std::exception& error) {
                        ReportException(state ? "Worker task" : "Worker initialization", error.what());
                    } catch (...) {
                        ReportException(state ? "Worker task" : "Worker initialization",
                                        "non-standard exception");
                    }
                    ++work_done;
                    wait_condition.notify_all();
                }
            }
            ++workers_stopped;
            wait_condition.notify_all();
        };
        threads.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            threads.emplace_back(lambda);
        }
    }

    StatefulThreadWorker& operator=(const StatefulThreadWorker&) = delete;
    StatefulThreadWorker(const StatefulThreadWorker&) = delete;

    StatefulThreadWorker& operator=(StatefulThreadWorker&&) = delete;
    StatefulThreadWorker(StatefulThreadWorker&&) = delete;

    void QueueWork(Task work) {
        {
            std::unique_lock lock{queue_mutex};
            requests.emplace(std::move(work));
            ++work_scheduled;
        }
        condition.notify_one();
    }

    void WaitForRequests(std::stop_token stop_token = {}) {
        std::stop_callback callback(stop_token, [this] {
            for (auto& thread : threads) {
                thread.request_stop();
            }
        });
        std::unique_lock lock{queue_mutex};
        wait_condition.wait(lock, [this] {
            return workers_stopped >= workers_queued || work_done >= work_scheduled;
        });
    }

private:
    void ReportException(const char* stage, const char* reason) const noexcept {
#ifdef NXBOX_UWP
        EdenXbox::CrashRendererException(stage, reason);
        EdenXbox::CrashRendererException("Worker name", thread_name);
#endif
        try {
            LOG_CRITICAL(Common, "{} [{}]: {}", stage, thread_name, reason);
        } catch (...) {
            // Logging may allocate; it must not replace the original worker failure.
        }
    }

    std::queue<Task> requests;
    std::mutex queue_mutex;
    std::condition_variable_any condition;
    std::condition_variable wait_condition;
    std::atomic<size_t> work_scheduled{};
    std::atomic<size_t> work_done{};
    std::atomic<size_t> workers_stopped{};
    std::atomic<size_t> workers_queued{};
    std::string thread_name;
    std::vector<std::jthread> threads;
};

using ThreadWorker = StatefulThreadWorker<>;

} // namespace Common
