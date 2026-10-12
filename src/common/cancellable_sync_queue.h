// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace Common {
// Callbacks run without the queue lock. Cancellation retires pending and future
// fences without running their callbacks, allowing a failed GPU to shut down.
class CancellableSyncQueue {
public:
    template <typename Callback>
    std::uint64_t Request(Callback&& callback) {
        const std::lock_guard lock{mutex};
        const auto fence = ++last;
        if (cancelled) {
            completed.store(fence, std::memory_order_release);
            cv.notify_all();
        } else {
            pending.push_back({fence, std::forward<Callback>(callback)});
        }
        return fence;
    }
    std::uint64_t Completed() const {
        return completed.load(std::memory_order_acquire);
    }
    void Wait(std::uint64_t fence) {
        std::unique_lock lock{mutex};
        cv.wait(lock, [&] { return Completed() >= fence; });
    }
    void Tick() {
        std::unique_lock lock{mutex};
        while (!pending.empty()) {
            auto request = std::move(pending.front());
            pending.pop_front();
            lock.unlock();
            // If this throws, unique_lock no longer owns the mutex. The renderer
            // boundary cancels the remaining requests and wakes CPU waiters.
            request.callback();
            lock.lock();
            completed.store(std::max(Completed(), request.fence), std::memory_order_release);
            cv.notify_all();
        }
    }
    void Cancel() {
        const std::lock_guard lock{mutex};
        cancelled = true;
        pending.clear();
        completed.store(last, std::memory_order_release);
        cv.notify_all();
    }

private:
    struct RequestEntry {
        std::uint64_t fence;
        std::function<void()> callback;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<RequestEntry> pending;
    std::atomic<std::uint64_t> completed{};
    std::uint64_t last{};
    bool cancelled{};
};
} // namespace Common
