// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace D3D12 {
// Jobs own immutable inputs. Only this map is shared with the requesting thread;
// expensive compilation never runs with its mutex held. One pool serves both stages.
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
    explicit AsyncPsoWorkerPool(size_t thread_count = 2, size_t capacity_ = 4)
        : capacity{capacity_} {
        try {
            for (size_t i = 0; i < std::clamp(thread_count, size_t{1}, size_t{3}); ++i)
                workers.emplace_back([this] { Run(); });
        } catch (...) {
            Stop();
            throw;
        }
    }
    ~AsyncPsoWorkerPool() {
        Stop();
    }
    Entry Lookup(const Key& key) {
        std::scoped_lock lock{mutex};
        const auto it = entries.find(key);
        return it == entries.end() ? Entry{Status::Missing, {}, {}} : it->second;
    }
    bool CanSubmit() {
        std::scoped_lock lock{mutex};
        return !stopping && pending < capacity;
    }
    bool Submit(const Key& key, Job job) {
        std::scoped_lock lock{mutex};
        if (stopping || pending >= capacity || entries.contains(key))
            return false;
        entries.emplace(key, Entry{});
        ++pending;
        queue.emplace_back(key, std::move(job));
        condition.notify_one();
        return true;
    }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
            for (const auto& item : queue) {
                entries.at(item.first) = {Status::Failed, {}, "Compilation cancelled"};
                --pending;
            }
            queue.clear();
        }
        condition.notify_all();
        for (auto& worker : workers)
            if (worker.joinable())
                worker.join();
    }

private:
    void Run() {
        for (;;) {
            std::pair<Key, Job> item;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                item = std::move(queue.front());
                queue.pop_front();
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
                std::scoped_lock lock{mutex};
                entries.at(item.first) = std::move(result);
                --pending;
            }
        }
    }
    size_t capacity, pending{};
    bool stopping{};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::pair<Key, Job>> queue;
    std::unordered_map<Key, Entry, Hash> entries;
    std::vector<std::thread> workers;
};
} // namespace D3D12
