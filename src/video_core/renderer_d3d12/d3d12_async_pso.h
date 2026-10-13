// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace D3D12 {
// Jobs own immutable inputs. Only this map is shared with the requesting thread;
// expensive compilation never runs with its mutex held. One pool serves both stages.
//
// A driver compile can block forever (BotW has compute shaders the Xbox driver never finishes),
// and a thread cannot be cancelled. A job running longer than `stall_limit` stops counting
// against the queue capacity and a replacement worker takes over, up to `max_threads`. Shutdown
// detaches every busy worker (any of them may never return); each keeps the shared state alive,
// and Stop() reports how many so the owner can keep what the jobs still reference.
template <typename Key, typename Result, typename Hash = std::hash<Key>>
class AsyncPsoWorkerPool {
public:
    enum class Status { Missing, Pending, Ready, Failed };
    struct Entry {
        Status status{Status::Pending};
        std::shared_ptr<Result> result;
        std::string error;
    };
    using Job = std::function<std::shared_ptr<Result>()>;
    using Clock = std::chrono::steady_clock;
    explicit AsyncPsoWorkerPool(size_t thread_count = 2, size_t capacity_ = 4,
                                Clock::duration stall_limit = std::chrono::seconds{5},
                                size_t max_threads = 6)
        : state{std::make_shared<State>()} {
        state->capacity = capacity_;
        state->stall_limit = stall_limit;
        base_threads = std::clamp(thread_count, size_t{1}, size_t{3});
        thread_limit = std::max(max_threads, base_threads);
        try {
            std::scoped_lock lock{state->mutex};
            for (size_t i = 0; i < base_threads; ++i)
                Spawn();
        } catch (...) {
            Stop();
            throw;
        }
    }
    ~AsyncPsoWorkerPool() {
        Stop();
    }
    Entry Lookup(const Key& key) {
        std::scoped_lock lock{state->mutex};
        const auto it = state->entries.find(key);
        return it == state->entries.end() ? Entry{Status::Missing, {}, {}} : it->second;
    }
    bool CanSubmit() {
        std::scoped_lock lock{state->mutex};
        return !state->stopping && Admittable();
    }
    bool Submit(const Key& key, Job job) {
        std::scoped_lock lock{state->mutex};
        if (state->stopping || !Admittable() || state->entries.contains(key))
            return false;
        state->entries.emplace(key, Entry{});
        ++state->pending;
        state->queue.emplace_back(key, std::move(job));
        state->condition.notify_one();
        return true;
    }
    /// Jobs that have run past the stall limit and still hold a worker.
    size_t Stalled() {
        std::scoped_lock lock{state->mutex};
        return CountStalled();
    }
    /// Returns how many busy workers were detached instead of joined.
    size_t Stop() {
        std::vector<bool> busy;
        size_t detached = 0;
        {
            std::scoped_lock lock{state->mutex};
            state->stopping = true;
            for (const auto& item : state->queue) {
                state->entries.at(item.first) = {Status::Failed, {}, "Compilation cancelled"};
                --state->pending;
            }
            state->queue.clear();
            for (const auto& since : state->busy_since)
                busy.push_back(since.has_value());
        }
        state->condition.notify_all();
        for (size_t i = 0; i < workers.size(); ++i) {
            if (!workers[i].joinable())
                continue;
            if (busy[i]) {
                workers[i].detach();
                ++detached;
            } else {
                workers[i].join();
            }
        }
        return detached;
    }

private:
    struct State {
        size_t capacity{}, pending{};
        Clock::duration stall_limit{};
        bool stopping{};
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<std::pair<Key, Job>> queue;
        std::unordered_map<Key, Entry, Hash> entries;
        std::vector<std::optional<Clock::time_point>> busy_since; // Indexed by worker.
    };

    // Caller holds the mutex.
    size_t CountStalled() const {
        const auto now = Clock::now();
        return static_cast<size_t>(std::count_if(
            state->busy_since.begin(), state->busy_since.end(),
            [&](const auto& since) { return since && now - *since >= state->stall_limit; }));
    }
    // Caller holds the mutex. Replaces stalled workers before deciding on admission.
    bool Admittable() {
        const size_t stalled = CountStalled();
        while (workers.size() - stalled < base_threads && workers.size() < thread_limit)
            Spawn();
        // With every worker stuck, an admitted job would stay Pending forever.
        return workers.size() > stalled && state->pending - stalled < state->capacity;
    }
    // Caller holds the mutex.
    void Spawn() {
        const size_t index = workers.size();
        // The new thread blocks on the mutex the caller holds until busy_since has its slot.
        workers.emplace_back([shared = state, index] { Run(*shared, index); });
        state->busy_since.emplace_back();
    }
    static void Run(State& state, size_t index) {
        for (;;) {
            std::pair<Key, Job> item;
            {
                std::unique_lock lock{state.mutex};
                state.condition.wait(lock, [&] { return state.stopping || !state.queue.empty(); });
                if (state.stopping)
                    return;
                item = std::move(state.queue.front());
                state.queue.pop_front();
                state.busy_since[index] = Clock::now();
            }
            Entry result;
            try {
                result.result = item.second();
                result.status = result.result ? Status::Ready : Status::Failed;
                if (!result.result)
                    result.error = "Compilation returned no pipeline";
            } catch (const std::exception& error) {
                result.status = Status::Failed;
                result.error = error.what();
            } catch (...) {
                result.status = Status::Failed;
                result.error = "Unknown pipeline compilation exception";
            }
            // Release captured snapshots before admitting more work.
            item.second = {};
            {
                std::scoped_lock lock{state.mutex};
                state.entries.at(item.first) = std::move(result);
                --state.pending;
                state.busy_since[index].reset();
            }
        }
    }
    std::shared_ptr<State> state;
    size_t base_threads{}, thread_limit{};
    std::vector<std::thread> workers;
};
} // namespace D3D12
