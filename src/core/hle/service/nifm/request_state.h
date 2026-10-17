// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace Service::NIFM {

// A request brings up a local interface, not a Nintendo authenticated session.
class RequestStateMachine final {
public:
    enum class State : std::uint32_t { NotSubmitted = 1, Accepted = 3 };
    enum class Outcome { NotSubmitted, CommunicationDisabled, Success };

    void Submit(bool local_connection) {
        state = local_connection ? State::Accepted : State::NotSubmitted;
        outcome = local_connection ? Outcome::Success : Outcome::CommunicationDisabled;
    }

    void Cancel() {
        state = State::NotSubmitted;
        outcome = Outcome::NotSubmitted;
    }

    bool Refresh(bool local_connection) {
        if (IsAccepted() && !local_connection) {
            Submit(false);
            return true;
        }
        return false;
    }

    bool IsAccepted() const {
        return state == State::Accepted;
    }

    State GetState() const {
        return state;
    }

    Outcome GetOutcome() const {
        return outcome;
    }

private:
    State state{State::NotSubmitted};
    Outcome outcome{Outcome::NotSubmitted};
};

} // namespace Service::NIFM
