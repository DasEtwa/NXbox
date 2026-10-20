// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/frontend/applets/error.h"

namespace EdenXbox {

// The dispatcher draws snapshots; only the session worker invokes guest callbacks.
class ErrorDisplay {
public:
    struct Message {
        std::uint64_t id;
        std::string code;
        std::string message;
    };

    void Show(Result error, std::string message,
              Core::Frontend::ErrorApplet::FinishedCallback finished);
    std::optional<Message> Current() const;
    bool Pending() const;
    void Dismiss(std::uint64_t id);
    void Close();
    void CompleteDismissed();
    void Clear();

private:
    struct Request {
        Message message;
        Core::Frontend::ErrorApplet::FinishedCallback finished;
        bool dismissed = false;
    };
    mutable std::mutex mutex_;
    std::deque<Request> requests_;
    std::uint64_t next_id_ = 0;
};

class ErrorApplet final : public Core::Frontend::ErrorApplet {
public:
    explicit ErrorApplet(std::shared_ptr<ErrorDisplay> display);
    void Close() const override;
    void ShowError(Result error, FinishedCallback finished) const override;
    void ShowErrorWithTimestamp(Result error, std::chrono::seconds time,
                                FinishedCallback finished) const override;
    void ShowCustomErrorText(Result error, std::string main_text, std::string detail_text,
                             FinishedCallback finished) const override;

private:
    std::shared_ptr<ErrorDisplay> display_;
};

} // namespace EdenXbox
