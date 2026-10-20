// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace D3D12 {
// Submission boundaries only. Publication/acknowledgement is independent of GPU
// completion, and failure always wakes both initialization and submission waiters.
class RecordingProgress {
public:
    RecordingProgress(std::atomic<uint64_t>& acknowledged, std::atomic<bool>& failed)
        : acknowledged{acknowledged}, failed{failed} {}
    void Ready() {
        {
            std::lock_guard lock{mutex};
            initialized = true;
        }
        changed.notify_all();
    }
    void WaitReady() {
        std::unique_lock lock{mutex};
        changed.wait(lock, [&] { return initialized; });
        ThrowFailure();
    }
    void Acknowledge(uint64_t tick) {
        {
            std::lock_guard lock{mutex};
            acknowledged.store(tick, std::memory_order_release);
        }
        changed.notify_all();
    }
    void Await(uint64_t tick) {
        std::unique_lock lock{mutex};
        changed.wait(lock, [&] {
            return acknowledged.load(std::memory_order_acquire) >= tick ||
                   failed.load(std::memory_order_acquire);
        });
        ThrowFailure();
    }
    void Fail(std::exception_ptr reason) {
        {
            std::lock_guard lock{mutex};
            if (!error)
                error = reason;
            initialized = true;
            failed.store(true, std::memory_order_release);
        }
        changed.notify_all();
    }
    void Check() {
        if (!failed.load(std::memory_order_acquire))
            return;
        std::lock_guard lock{mutex};
        ThrowFailure();
    }

private:
    void ThrowFailure() {
        if (error)
            std::rethrow_exception(error);
        if (failed.load(std::memory_order_acquire))
            throw std::runtime_error("D3D12 recorder submission failed");
    }
    std::atomic<uint64_t>& acknowledged;
    std::atomic<bool>& failed;
    std::mutex mutex;
    std::condition_variable changed;
    std::exception_ptr error;
    bool initialized{};
};
} // namespace D3D12
