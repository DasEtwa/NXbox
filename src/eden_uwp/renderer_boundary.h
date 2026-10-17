// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <exception>
#include <utility>

namespace EdenXbox {
class UnknownRendererException final : public std::exception {
public:
    explicit UnknownRendererException(const char* stage_) noexcept : stage{stage_} {}
    const char* what() const noexcept override {
        return stage;
    }

private:
    const char* stage;
};

// A reporter returning false retains the other backends' existing exception policy.
// Unknown exception diagnostics require no allocation, including under memory pressure.
template <typename Operation, typename Reporter>
bool RendererBoundary(const char* stage, Operation&& operation, Reporter&& reporter) {
    try {
        std::forward<Operation>(operation)();
        return true;
    } catch (const std::exception& error) {
        if (!reporter(error))
            throw;
    } catch (...) {
        const UnknownRendererException error{stage};
        if (!reporter(error))
            throw;
    }
    return false;
}
} // namespace EdenXbox
