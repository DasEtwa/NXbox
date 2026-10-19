// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/error_applet.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace EdenXbox {

void ErrorDisplay::Show(Result error, std::string message,
                        Core::Frontend::ErrorApplet::FinishedCallback finished) {
    std::ostringstream code;
    code << std::setfill('0') << std::setw(4) << (2000 + static_cast<unsigned>(error.GetModule()))
         << '-' << std::setw(4) << error.GetDescription();
    std::scoped_lock lock{mutex_};
    requests_.push_back({{++next_id_, code.str(), std::move(message)}, std::move(finished)});
}

std::optional<ErrorDisplay::Message> ErrorDisplay::Current() const {
    std::scoped_lock lock{mutex_};
    if (requests_.empty() || requests_.front().dismissed) {
        return std::nullopt;
    }
    return requests_.front().message;
}

bool ErrorDisplay::Pending() const {
    std::scoped_lock lock{mutex_};
    return !requests_.empty();
}

void ErrorDisplay::Dismiss(std::uint64_t id) {
    std::scoped_lock lock{mutex_};
    if (!requests_.empty() && requests_.front().message.id == id) {
        requests_.front().dismissed = true;
    }
}

void ErrorDisplay::Close() {
    std::scoped_lock lock{mutex_};
    for (auto& request : requests_) {
        request.dismissed = true;
    }
}

void ErrorDisplay::CompleteDismissed() {
    for (;;) {
        Core::Frontend::ErrorApplet::FinishedCallback finished;
        {
            std::scoped_lock lock{mutex_};
            if (requests_.empty() || !requests_.front().dismissed) {
                return;
            }
            finished = std::move(requests_.front().finished);
            requests_.pop_front();
        }
        // Remove before invoking, and never hold the UI mutex across guest code.
        if (finished) {
            finished();
        }
    }
}

void ErrorDisplay::Clear() {
    std::scoped_lock lock{mutex_};
    requests_.clear();
}

ErrorApplet::ErrorApplet(std::shared_ptr<ErrorDisplay> display) : display_{std::move(display)} {}

void ErrorApplet::Close() const {
    display_->Close();
}

void ErrorApplet::ShowError(Result error, FinishedCallback finished) const {
    display_->Show(error, {}, std::move(finished));
}

void ErrorApplet::ShowErrorWithTimestamp(Result error, std::chrono::seconds,
                                         FinishedCallback finished) const {
    display_->Show(error, {}, std::move(finished));
}

void ErrorApplet::ShowCustomErrorText(Result error, std::string main_text, std::string detail_text,
                                      FinishedCallback finished) const {
    if (!main_text.empty() && !detail_text.empty()) {
        main_text += "\n\n";
    }
    main_text += detail_text;
    display_->Show(error, std::move(main_text), std::move(finished));
}

} // namespace EdenXbox
