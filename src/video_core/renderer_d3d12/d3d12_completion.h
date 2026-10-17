// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <vector>

namespace D3D12 {
// Detach both lists before invoking callbacks. Reentrant waits cannot replay this
// batch, and resources recorded by a callback belong to the next submission.
template <typename Resource>
void CompleteBatch(std::vector<std::function<void()>>& pending, std::vector<Resource>& retained) {
    std::vector<std::function<void()>> callbacks;
    std::vector<Resource> resources;
    callbacks.swap(pending);
    resources.swap(retained);
    for (auto& callback : callbacks) {
        callback();
    }
}
} // namespace D3D12
