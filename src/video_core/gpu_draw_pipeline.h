// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace VideoCommon {

// One renderer consumer; capacity includes the executing job. Never hold a cache,
// guest-memory lock while publishing or waiting for this queue.
class GpuDrawPipeline {
public:
    explicit GpuDrawPipeline(std::size_t capacity_ = 64) : capacity{capacity_} {
        if (capacity == 0)
            throw std::invalid_argument("GPU pipeline capacity must be positive");
        worker = std::thread{[this] { Run(); }};
    }
    ~GpuDrawPipeline() {
        Close();
    }
    void Close() {
        {
            std::lock_guard lock{mutex};
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable())
            worker.join();
    }
    GpuDrawPipeline(const GpuDrawPipeline&) = delete;
    GpuDrawPipeline& operator=(const GpuDrawPipeline&) = delete;

    bool IsConsumer() const noexcept {
        return current == this;
    }
    void Push(std::function<void()> job) {
        std::unique_lock lock{mutex};
        changed.wait(lock, [this] { return pending < capacity || failure || stopping; });
        Rethrow();
        if (stopping)
            throw std::runtime_error("GPU pipeline is stopping");
        jobs.push_back(std::move(job));
        ++pending;
        changed.notify_all();
    }
    void Drain() {
        if (IsConsumer())
            return;
        std::unique_lock lock{mutex};
        changed.wait(lock, [this] { return pending == 0; });
        Rethrow();
    }
    template <class Func>
    auto Invoke(Func&& function) -> std::invoke_result_t<Func> {
        if (IsConsumer())
            return std::forward<Func>(function)();
        using Result = std::invoke_result_t<Func>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Func>(function));
        auto result = task->get_future();
        // Only the queued closure retains the task. Cancelling that closure must
        // break its promise and wake result.get(), even if execution never starts.
        Push([task = std::move(task)] { (*task)(); });
        return result.get();
    }

private:
    void Rethrow() const {
        if (failure)
            std::rethrow_exception(failure);
    }
    void Run() noexcept {
        current = this;
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty())
                    break;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            try {
                job();
            } catch (...) {
                std::lock_guard lock{mutex};
                failure = std::current_exception();
                // Destroy cancelled synchronous tasks to wake their futures too.
                pending -= jobs.size();
                jobs.clear();
            }
            {
                std::lock_guard lock{mutex};
                --pending;
            }
            changed.notify_all();
        }
        current = nullptr;
    }
    inline static thread_local const GpuDrawPipeline* current{};
    const std::size_t capacity;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> jobs;
    std::size_t pending{};
    bool stopping{};
    std::exception_ptr failure;
    std::thread worker;
};

// DMA may otherwise expose a direct guest-memory span. Decode an owned segment
// so host draw building cannot race subsequent reads of command words.
template <class Word>
std::span<const Word> SnapshotGpuMethods(std::span<const Word> commands,
                                         std::vector<Word>& storage) {
    storage.assign(commands.begin(), commands.end());
    return storage;
}

// Own every rasterizer-visible value, including inline indices and HLE shader
// replacements. The consumer's unconsumed flags must survive the next snapshot.
// Tables and engine execution/macro machinery are not copied per draw.
template <class Engine>
struct GpuDrawSnapshot {
    decltype(Engine::regs) regs;
    decltype(Engine::state) state;
    decltype(Engine::draw_manager) draw_manager;
    decltype(Engine::dirty.flags) dirty_flags;
    decltype(Engine::replace_table) replacements;
    decltype(Engine::engine_state) engine_hint;

    void Apply(Engine& target) const {
        target.regs = regs;
        target.state = state;
        target.draw_manager = draw_manager;
        target.dirty.flags |= dirty_flags;
        target.replace_table = replacements;
        target.engine_state = engine_hint;
    }
};

template <class Engine>
GpuDrawSnapshot<Engine> CaptureDrawSnapshot(Engine& source) {
    GpuDrawSnapshot<Engine> result{source.regs,        source.state,         source.draw_manager,
                                   source.dirty.flags, source.replace_table, source.engine_state};
    source.dirty.flags.reset();
    return result;
}

} // namespace VideoCommon
