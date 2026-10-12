// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <exception>
#include <utility>

namespace Common {
// Generated machine code has no C++ unwind metadata. Capture at the native
// callback and rethrow only after returning to a compiled C++ frame.
class DeferredException {
public:
    template <typename Callback>
    void Invoke(Callback&& callback) noexcept {
        if (error)
            return;
        try {
            std::forward<Callback>(callback)();
        } catch (...) {
            error = std::current_exception();
        }
    }
    bool HasError() const noexcept {
        return error != nullptr;
    }
    void Rethrow() {
        if (auto pending = std::exchange(error, {}))
            std::rethrow_exception(pending);
    }

private:
    std::exception_ptr error;
};
} // namespace Common
