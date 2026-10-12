// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NxboxShaderProbe {
using Hash = std::uint64_t;

inline std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

inline bool Number(std::string_view text, Hash& value, int base = 16) {
    text = Trim(text);
    if (base == 16 && (text.starts_with("0x") || text.starts_with("0X"))) {
        text.remove_prefix(2);
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

struct Selection {
    std::vector<std::pair<Hash, Hash>> ranges;
    bool Parse(std::string_view text, bool allow_ranges = true) {
        std::vector<std::pair<Hash, Hash>> parsed;
        while (!text.empty()) {
            const auto comma = text.find(',');
            const auto item = Trim(text.substr(0, comma));
            const auto dash = item.find('-');
            Hash first{}, last{};
            if (!Number(item.substr(0, dash), first)) {
                return false;
            }
            last = first;
            if (dash != item.npos &&
                (!allow_ranges || !Number(item.substr(dash + 1), last) || last < first)) {
                return false;
            }
            parsed.emplace_back(first, last);
            if (comma == text.npos) {
                break;
            }
            text.remove_prefix(comma + 1);
            if (Trim(text).empty()) {
                return false;
            }
        }
        ranges = std::move(parsed);
        return true;
    }
    bool Contains(Hash hash) const {
        return std::any_of(ranges.begin(), ranges.end(), [hash](const auto& range) {
            return hash >= range.first && hash <= range.second;
        });
    }
};

struct Bucket {
    Hash seed{}, index{}, count{};
    bool Parse(std::string_view text) {
        const auto colon = text.find(':');
        const auto slash = text.find('/');
        Hash s{}, k{}, n{};
        if (colon == text.npos || slash == text.npos || slash <= colon ||
            !Number(text.substr(0, colon), s) ||
            !Number(text.substr(colon + 1, slash - colon - 1), k, 10) ||
            !Number(text.substr(slash + 1), n, 10) || n == 0 || k >= n) {
            return false;
        }
        seed = s;
        index = k;
        count = n;
        return true;
    }
    bool Contains(Hash hash) const {
        return count != 0 && (hash ^ seed) % count == index;
    }
};

struct Cycle {
    Hash count{}, duration_ms{}, outer_count{}, outer_index{};
    bool Parse(std::string_view text) {
        const auto colon = text.find(':');
        Hash n{}, seconds{};
        if (colon == text.npos || !Number(text.substr(0, colon), n, 10) || n == 0 ||
            n == std::numeric_limits<Hash>::max() || !Number(text.substr(colon + 1), seconds, 10) ||
            seconds == 0 || seconds > std::numeric_limits<Hash>::max() / 2000) {
            return false;
        }
        count = n;
        duration_ms = seconds * 1000;
        return true;
    }
    bool ParseWithin(std::string_view text) {
        const auto colon = text.find(':');
        Hash n{}, k{};
        if (colon == text.npos || !Number(text.substr(0, colon), n, 10) || n == 0 ||
            !Number(text.substr(colon + 1), k, 10) || k >= n) {
            return false;
        }
        outer_count = n;
        outer_index = k;
        return true;
    }
    bool Contains(Hash hash, Hash phase) const {
        if (count == 0 || phase == 0 || phase > count) {
            return false;
        }
        if (outer_count != 0) {
            if (hash % outer_count != outer_index) {
                return false;
            }
            // Split the outer bucket's quotient, rather than reusing correlated low bits.
            hash /= outer_count;
        }
        return hash % count == phase - 1;
    }
};

// Render-thread state; phase changes only at the first guest draw of a frame.
struct CycleState {
    Hash origin_tick{}, start_tick{}, ordinal{}, phase{};
    bool started{}, active{}, dumped{}, frame_begun{};
    bool Advance(const Cycle& cycle, Hash tick) {
        if (cycle.count == 0) {
            return false;
        }
        if (!started) {
            started = true;
            origin_tick = tick;
        }
        const Hash elapsed = tick >= origin_tick ? tick - origin_tick : 0;
        const Hash warmup = cycle.duration_ms * 2;
        if (elapsed < warmup) {
            return false;
        }
        const Hash next = (elapsed - warmup) / cycle.duration_ms;
        if (active && next == ordinal) {
            return false;
        }
        active = true;
        dumped = false;
        ordinal = next;
        phase = ordinal % (cycle.count + 1);
        start_tick = tick - (elapsed - warmup) % cycle.duration_ms;
        return true;
    }
    bool TakeProbe(const Cycle& cycle, Hash tick) {
        if (!active || dumped || tick < start_tick || tick - start_tick < cycle.duration_ms / 2 ||
            tick - start_tick >= cycle.duration_ms) {
            return false;
        }
        dumped = true;
        return true;
    }
};

inline std::string Hex(Hash value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

struct Config {
    bool ids{};
    Hash every{60};
    Selection skip, dump;
    Bucket bucket;
    Cycle cycle;
    bool SkipEnabled() const {
        return !skip.ranges.empty() || bucket.count != 0 || cycle.count != 0;
    }
    bool DrawEnabled() const {
        return ids || SkipEnabled();
    }
    bool Skip(Hash hash, Hash phase = 0) const {
        if (cycle.count != 0) {
            return cycle.Contains(hash, phase);
        }
        return skip.Contains(hash) || bucket.Contains(hash);
    }
    bool Dump(Hash pipeline, const std::array<Hash, 6>& stages) const {
        if (dump.ranges.empty()) {
            return false;
        }
        return dump.Contains(pipeline) || std::any_of(stages.begin(), stages.end(), [this](Hash h) {
                   return h != 0 && dump.Contains(h);
               });
    }
};

// Owned by the render thread. Only sampled frames allocate/count; each report is a complete set.
struct Frames {
    struct Counts {
        std::array<Hash, 6> stages;
        Hash drawn{}, skipped{};
    };
    Hash frame{};
    std::map<Hash, Counts> pipelines;
    void Record(const Config& config, Hash hash, const std::array<Hash, 6>& stages, bool skipped) {
        if (!config.ids || frame % config.every != 0) {
            return;
        }
        auto& counts = pipelines.try_emplace(hash, Counts{stages, 0, 0}).first->second;
        ++(skipped ? counts.skipped : counts.drawn);
    }
    template <typename Log>
    void End(const Config& config, Log&& log) {
        if (config.ids && frame % config.every == 0) {
            log("SHADER_IDS frame=" + std::to_string(frame) +
                " pipelines=" + std::to_string(pipelines.size()));
            // One bounded physical line per pipeline; no truncation of large frame sets.
            for (const auto& [hash, counts] : pipelines) {
                std::string line = "SHADER_IDS frame=" + std::to_string(frame) + " p=" + Hex(hash) +
                                   " d=" + std::to_string(counts.drawn) +
                                   " skip=" + std::to_string(counts.skipped) + " s=";
                for (std::size_t i = 0; i < counts.stages.size(); ++i) {
                    if (i != 0) {
                        line += ',';
                    }
                    line += counts.stages[i] == 0 ? "-" : Hex(counts.stages[i]);
                }
                log(line);
            }
            pipelines.clear();
        }
        ++frame;
    }
};
} // namespace NxboxShaderProbe
