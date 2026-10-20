// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace D3D12 {
// One producer and one consumer. Only chunk boundaries synchronize; neither a command
// nor its variable payload needs to fit in one chunk. Published bytes are immutable.
template <size_t ChunkSize = 256 * 1024, size_t ChunkCount = 32>
class RecordStream {
public:
    static_assert(ChunkCount >= 2 && ChunkSize > 0);
    static constexpr size_t chunk_size = ChunkSize, chunk_count = ChunkCount;
    using Replay = void (*)(void*, const std::byte*, bool);
    struct Header {
        Replay replay;
        size_t size;
    };
    // More room absorbs bursts of short submissions while the native recorder catches up.
    // The bound still applies to payloads and COM holds; a slow consumer backpressures safely.
    explicit RecordStream(size_t capacity = ChunkCount) : active_chunks{capacity} {
        if (capacity < 2 || capacity > ChunkCount)
            throw std::invalid_argument("invalid D3D12 recording capacity");
        chunks = std::make_unique<Chunk[]>(capacity);
    }
    RecordStream(const RecordStream&) = delete;
    void Write(const void* source, size_t size) {
        auto* bytes = static_cast<const std::byte*>(source);
        while (size) {
            auto& chunk = chunks[write_index];
            if (!write_offset) {
                auto used = chunk.used.load(std::memory_order_acquire);
                if (used) {
                    BoundaryWait([&] {
                        do {
                            chunk.used.wait(used, std::memory_order_acquire);
                            used = chunk.used.load(std::memory_order_acquire);
                        } while (used);
                    });
                }
            }
            const auto count = std::min(size, ChunkSize - write_offset);
            std::memcpy(chunk.data.data() + write_offset, bytes, count);
            write_offset += count;
            bytes += count;
            size -= count;
            if (write_offset == ChunkSize)
                Flush();
        }
    }
    void Flush() {
        if (!write_offset)
            return;
        chunks[write_index].used.store(write_offset, std::memory_order_release);
        write_offset = 0;
        if (++write_index == active_chunks)
            write_index = 0;
        available.fetch_add(1, std::memory_order_release);
        available.notify_one();
    }
    // Called only after the producer has finished writing. Wakes the consumer at EOF.
    void Close() {
        Flush();
        closed.store(true, std::memory_order_release);
        available.fetch_add(1, std::memory_order_release);
        available.notify_one();
    }
    bool Consume(void* target, bool execute = true) {
        if (!pending_header) {
            Header header{};
            if (!Read(&header, sizeof(header)))
                return false;
            pending_header = header;
        }
        // A failed scratch allocation must leave the consumed header available to the
        // failure drain. Otherwise the next Consume interprets payload bytes as a thunk.
        storage.resize((pending_header->size + sizeof(std::max_align_t) - 1) /
                       sizeof(std::max_align_t));
        if (!Read(storage.data(), pending_header->size))
            throw std::runtime_error("truncated D3D12 recording");
        const auto header = *pending_header;
        pending_header.reset(); // Replay releases holds even on exceptions; never retry it.
        header.replay(target, reinterpret_cast<const std::byte*>(storage.data()), execute);
        return true;
    }
    template <class Command>
    void Begin(size_t extra = 0) {
        const Header header{[](void* target, const std::byte* data, bool execute) {
                                Command command;
                                std::memcpy(&command, data, sizeof(command));
                                // Even failed/abandoned recordings release all encoded COM holds.
                                try {
                                    if (execute)
                                        command.Replay(target, data);
                                } catch (...) {
                                    command.Release(data);
                                    throw;
                                }
                                command.Release(data);
                            },
                            sizeof(Command) + extra};
        Write(&header, sizeof(header));
    }
    template <class Wait>
    void BoundaryWait(Wait wait) {
        const auto start = metrics ? Clock() : 0;
        wait();
        if (metrics) {
            ++waits;
            wait_ns += Clock() - start;
        }
    }
    bool metrics{};
    uint64_t waits{}, wait_ns{}; // Producer only; sampled by the producer's frame profile.
private:
    static uint64_t Clock() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }
    bool Read(void* destination, size_t size) {
        auto* bytes = static_cast<std::byte*>(destination);
        while (size) {
            auto& chunk = chunks[read_index];
            auto used = chunk.used.load(std::memory_order_acquire);
            while (!used) {
                const auto generation = available.load(std::memory_order_acquire);
                used = chunk.used.load(std::memory_order_acquire);
                if (used)
                    break;
                if (closed.load(std::memory_order_acquire)) {
                    used = chunk.used.load(std::memory_order_acquire);
                    if (!used)
                        return false;
                    break;
                }
                available.wait(generation, std::memory_order_acquire);
            }
            const auto count = std::min(size, used - read_offset);
            std::memcpy(bytes, chunk.data.data() + read_offset, count);
            bytes += count;
            size -= count;
            read_offset += count;
            if (read_offset == used) {
                read_offset = 0;
                if (++read_index == active_chunks)
                    read_index = 0;
                chunk.used.store(0, std::memory_order_release);
                chunk.used.notify_one();
            }
        }
        return true;
    }
    struct Chunk {
        std::atomic<size_t> used{};
        std::array<std::byte, ChunkSize> data;
    };
    std::unique_ptr<Chunk[]> chunks;
    const size_t active_chunks;
    size_t write_index{}, write_offset{}, read_index{}, read_offset{};
    std::atomic<uint64_t> available{};
    std::atomic<bool> closed{};
    std::optional<Header> pending_header;  // Consumer only; survives scratch allocation failure.
    std::vector<std::max_align_t> storage; // Consumer only; no producer allocation.
};
} // namespace D3D12
