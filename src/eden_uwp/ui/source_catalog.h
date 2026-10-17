// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure catalog operations. Index fetching, local scanning and rendering belong to the screen.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "eden_uwp/ui/tinfoil_index.h"

namespace EdenXbox::Tinfoil {
enum class Tab { Games, Updates, Dlc, All };
enum class Sort { Name, Size, Newest };
struct Game {
    std::string base_id;
    std::string name;
    std::vector<std::size_t> items;
    std::uint64_t size = 0;
    std::size_t newest = 0;
};

inline bool Matches(Kind kind, Tab tab) {
    return tab == Tab::All || (tab == Tab::Games && kind == Kind::Game) ||
           (tab == Tab::Updates && kind == Kind::Update) || (tab == Tab::Dlc && kind == Kind::Dlc);
}

inline std::vector<Game> Group(const std::vector<Package>& packages,
                               const std::vector<std::uint64_t>& sizes) {
    std::vector<Game> games;
    std::map<std::string, std::size_t> groups;
    for (std::size_t i = 0; i < packages.size(); ++i) {
        const auto& package = packages[i];
        // Unknown IDs remain separate: equal names do not prove that two files are one game.
        const auto key = package.base_id.empty() ? "file:" + std::to_string(i) : package.base_id;
        const auto [it, inserted] = groups.emplace(key, games.size());
        if (inserted)
            games.push_back({package.base_id, package.name, {}, 0, i});
        auto& game = games[it->second];
        if (package.kind == Kind::Game)
            game.name = package.name;
        game.items.push_back(i);
        const auto bytes = i < sizes.size() ? sizes[i] : 0;
        game.size += std::min(bytes, std::numeric_limits<std::uint64_t>::max() - game.size);
        game.newest = i;
    }
    return games;
}

// The index has no publication timestamps. Newest uses reverse source order, never version
// numbers (versions of different games are not comparable).
inline std::vector<std::size_t> Select(const std::vector<Game>& games,
                                       const std::vector<Package>& packages, Tab tab, Sort sort) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < games.size(); ++i) {
        if (std::any_of(games[i].items.begin(), games[i].items.end(),
                        [&](auto item) { return Matches(packages[item].kind, tab); }))
            result.push_back(i);
    }
    std::stable_sort(result.begin(), result.end(), [&](auto a, auto b) {
        if (sort == Sort::Size && games[a].size != games[b].size)
            return games[a].size > games[b].size;
        if (sort == Sort::Newest && games[a].newest != games[b].newest)
            return games[a].newest > games[b].newest;
        return detail::Lower(games[a].name) < detail::Lower(games[b].name);
    });
    return result;
}

inline bool IsInstalled(const Package& remote, const Package& local) {
    return !remote.title_id.empty() && remote.title_id == local.title_id &&
           remote.kind == local.kind &&
           (remote.kind == Kind::Game || !remote.version ||
            (local.version && *local.version >= *remote.version));
}

// Includes the active head in the limit. Items are copies so reloads and source changes cannot
// invalidate a waiting download. The screen starts only Front(), then removes it after joining.
template <typename Item>
class DownloadQueue {
public:
    static constexpr std::size_t kLimit = 8;
    bool Add(const Item& item) {
        if (items_.size() >= kLimit ||
            std::any_of(items_.begin(), items_.end(), [&](const auto& old) {
                return old.url == item.url || old.key == item.key;
            }))
            return false;
        items_.push_back(item);
        return true;
    }
    void Remove(std::size_t index) {
        if (index < items_.size())
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    const std::vector<Item>& Items() const {
        return items_;
    }
    bool Empty() const {
        return items_.empty();
    }
    const Item& Front() const {
        return items_.front();
    }

private:
    std::vector<Item> items_;
};
} // namespace EdenXbox::Tinfoil
