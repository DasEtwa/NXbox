"""Compile and execute portable production trace/lifecycle policies in /tmp."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r"""
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include "core/hle/service/service_trace.h"
#include "core/hle/service/service_interface_name.h"
#include "core/hle/service/nifm/request_state.h"

namespace First { struct IRequest {}; }
namespace Second { struct IRequest {}; }

int main() {
    constexpr auto first_interface = Service::ServiceInterfaceName<First::IRequest>();
    constexpr auto second_interface = Service::ServiceInterfaceName<Second::IRequest>();
    static_assert(first_interface == "First::IRequest");
    static_assert(second_interface == "Second::IRequest");
    Service::ServiceTrace disabled{false};
    assert(!disabled.RecordCall("acc:u0", false, 0, 0, false));
    assert(disabled.RecordMissing("acc:u0", false, 99));
    assert(!disabled.RecordMissing("acc:u0", false, 99));
    assert(disabled.RecordMissing("acc:u1", false, 99));
    assert(disabled.RecordMissing("acc:u0", true, 99));

    Service::ServiceTrace trace{true};
    std::atomic<int> first_calls{0};
    std::atomic<int> first_missing{0};
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 8; ++thread) {
        workers.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                first_calls += trace.RecordCall("acc:u0", false, 100, 0, false);
                first_missing += trace.RecordMissing("acc:u0", false, 100);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(first_calls == 1 && first_missing == 1);
    assert(trace.RecordCall("acc:u0", false, 100, 0xf601, false));
    assert(!trace.RecordCall("acc:u0", false, 100, 0xf601, false));
    assert(trace.RecordCall("acc:u0", true, 100, 0, false));
    assert(trace.RecordCall("acc:u1", false, 100, 0, false));
    assert(trace.RecordCall("acc:u0", false, 101, 0, false));
    assert(trace.RecordCall("acc:u0", false, 100, std::nullopt, true));
    assert(!trace.RecordCall("acc:u0", false, 100, std::nullopt, true));
    assert(trace.RecordCall("acc:u0", false, 100, std::nullopt, false));
    assert(trace.RecordCall("IRequest", false, 0, 0, false, first_interface));
    assert(trace.RecordCall("IRequest", false, 0, 0, false, second_interface));
    assert(!trace.RecordCall("IRequest", false, 0, 0, false, first_interface));
    assert(trace.RecordMissing("IRequest", false, 0, first_interface));
    assert(trace.RecordMissing("IRequest", false, 0, second_interface));
    assert(!trace.RecordMissing("IRequest", false, 0, first_interface));
    Service::ServiceTrace fresh_system{true};
    assert(fresh_system.RecordCall("acc:u0", false, 100, 0, false));
    assert(fresh_system.RecordMissing("acc:u0", false, 100));

    using Request = Service::NIFM::RequestStateMachine;
    Request request;
    assert(request.GetState() == Request::State::NotSubmitted);
    assert(request.GetOutcome() == Request::Outcome::NotSubmitted);
    // A host IP alone must not turn an unsubmitted request into a success.
    assert(!request.Refresh(true));
    assert(!request.IsAccepted());
    request.Submit(false);
    assert(request.GetOutcome() == Request::Outcome::CommunicationDisabled);
    assert(!request.IsAccepted());
    // Failed requests stay failed until explicitly resubmitted.
    assert(!request.Refresh(true));
    request.Submit(true);
    assert(request.IsAccepted());
    assert(request.GetOutcome() == Request::Outcome::Success);
    assert(!request.Refresh(true));
    request.Submit(true);
    assert(request.IsAccepted());
    assert(request.Refresh(false));
    assert(!request.IsAccepted());
    assert(request.GetOutcome() == Request::Outcome::CommunicationDisabled);
    assert(!request.Refresh(false));
    request.Submit(true);
    request.Cancel();
    assert(!request.IsAccepted());
    assert(request.GetOutcome() == Request::Outcome::NotSubmitted);
    request.Cancel();
    request.Submit(true);
    assert(request.IsAccepted());
}
"""


class RuntimeTests(unittest.TestCase):
    def test_trace_concurrency_and_network_lifecycle(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "A C++ compiler is required for the T16 runtime check")
        with tempfile.TemporaryDirectory(prefix="nxbox-t16-", dir="/tmp") as temporary:
            source = Path(temporary) / "service_runtime.cpp"
            binary = Path(temporary) / "service_runtime"
            source.write_text(HARNESS)
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_dispatch_captures_response_not_request_payload(self):
        service = (ROOT / "src/core/hle/service/service.cpp").read_text()
        ipc = (ROOT / "src/core/hle/service/ipc_helpers.h").read_text()
        self.assertIn("ctx.has_response = true;", ipc)
        self.assertEqual(service.count("ctx.has_response = false;"), 2)
        self.assertIn("ctx.HasResponse() && !deferred", service)
        self.assertIn("ctx.GetDataPayloadOffset()", service)
        self.assertEqual(service.count("TraceRequest(ctx, command);"), 4)
        self.assertIn("IPC::ResultNotSupported", service)
        self.assertIn("NXBOX_SERVICE_MISSING", service)
        self.assertIn("interface_name{interface_name_}", service)


if __name__ == "__main__":
    unittest.main()
