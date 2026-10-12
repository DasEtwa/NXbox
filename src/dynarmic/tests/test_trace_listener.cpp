// SPDX-License-Identifier: 0BSD
#include <cstdio>

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

class TestTraceListener final : public Catch::EventListenerBase {
public:
    using EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override {
        std::printf("Running test: %s\n", info.name.c_str());
        std::fflush(stdout);
    }
};

CATCH_REGISTER_LISTENER(TestTraceListener)
