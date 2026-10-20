# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production AM/FS trace adapter without a console or full build."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/core/hle/service/am/polling_service.h"

STUB = r"""
#pragma once
#include <cstdint>
#include <string_view>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
struct Result { u32 raw; };
namespace Core { class System {}; }
namespace Kernel {
class KServerSession {};
struct KThread { u64 id = 82; u64 GetThreadId() { return id; } };
}
namespace Service {
struct HLERequestContext {
    Kernel::KThread thread;
    bool response = true, deferred = false;
    u32 buffer[2] = {0xdeadbeef, 0x1234};
    int dispatches = 0, buffer_reads = 0;
    Kernel::KThread& GetThread() { return thread; }
    u32 GetCommand() { return 7; }
    u32 GetCommandType() { return 4; }
    bool HasResponse() { return response; }
    bool GetIsDeferred() { return deferred; }
    u32 GetDataPayloadOffset() { return 1; }
    u32* CommandBuffer() { ++buffer_reads; return buffer; }
};
struct SessionRequestHandler {
    virtual ~SessionRequestHandler() = default;
    virtual Result HandleSyncRequest(Kernel::KServerSession&, HLERequestContext&) = 0;
};
}
"""
LOGGER = r"""
#pragma once
#include <sstream>
#include <string>
#include <vector>
inline std::vector<std::string> records;
template <typename... Args> void Record(const char* format, const Args&... args) {
    std::ostringstream out;
    out << format;
    ((out << '|' << args), ...);
    records.push_back(out.str());
}
#define LOG_WARNING(category, ...) Record(__VA_ARGS__)
"""
DRIVER = r"""
#include <cassert>
#include "core/hle/service/am/polling_service.h"
// Use the real CRTP framework and handler table. Replace only the kernel/IPC
// execution boundary, so base-pointer conversions and handler invocation run.
namespace Service {
ServiceFrameworkBase::ServiceFrameworkBase(Core::System& system_, const char* name,
    u32 sessions, InvokerFn* invoker, std::string_view interface)
    : interface_name{interface}, system{system_}, service_name{name},
      is_i_storage{false}, handler_invoker{invoker}, max_sessions{sessions} {}
ServiceFrameworkBase::~ServiceFrameworkBase() = default;
void ServiceFrameworkBase::RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        handlers.emplace(functions[i].expected_header, functions[i]);
}
Result ServiceFrameworkBase::HandleSyncRequest(Kernel::KServerSession&, HLERequestContext& ctx) {
    ++ctx.dispatches;
    handler_invoker(this, handlers.at(ctx.GetCommand()).handler_callback, ctx);
    return {0xab};
}
}
struct Endpoint final : Service::PollingServiceFramework<Endpoint> {
    Endpoint(Core::System& system) : PollingServiceFramework{system, "IStorage"} {
        static const FunctionInfo functions[] = {{7, &Endpoint::Read, "Read"}};
        RegisterHandlers(functions);
    }
    void Read(Service::HLERequestContext& ctx) { ctx.buffer[1] = 0x1234; }
};
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string_view mode{argv[1]};
    Core::System system;
    Kernel::KServerSession session;
    Service::HLERequestContext ctx;
    Endpoint endpoint{system};
    if (mode == "other") ctx.thread.id = 83;
    if (mode == "limit")
        Service::AmFsPolling::sequence.store(Service::AmFsPolling::RequestLimit - 1);
    // Identical calls must both be retained; tracing must never skip guest work.
    assert(endpoint.HandleSyncRequest(session, ctx).raw == 0xab);
    assert(endpoint.HandleSyncRequest(session, ctx).raw == 0xab);
    assert(ctx.dispatches == 2);
    if (mode == "disabled" || mode == "other") {
        assert(records.empty() && ctx.buffer_reads == 0);
        assert(Service::AmFsPolling::sequence == 0);
        return 0;
    }
    if (mode == "limit") {
        assert(records.size() == 3 && ctx.buffer_reads == 1);
        assert(records.back().find("truncated") != std::string::npos);
        return 0;
    }
    assert(records.size() == 4 && ctx.buffer_reads == 2);
    assert(records[0].find("begin") != std::string::npos);
    assert(records[0].find("|1|82|IStorage|4|7") != std::string::npos);
    assert(records[2].find("|2|82|IStorage|4|7") != std::string::npos);
    // Decimal representation in the stub logger: result=0x1234, transport=0xab.
    assert(records[1].ends_with("|1|4660|0|171"));
    ctx.deferred = true;
    endpoint.HandleSyncRequest(session, ctx);
    assert(ctx.buffer_reads == 2 && records.back().ends_with("|0|0|1|171"));
    ctx.deferred = false;
    ctx.response = false;
    endpoint.HandleSyncRequest(session, ctx);
    assert(ctx.buffer_reads == 2 && records.back().ends_with("|0|0|0|171"));
    assert(ctx.dispatches == 4);
}
"""


class AmFsPollTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="nxbox-am-fs-", dir="/tmp")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        for name, contents in {
            "core/hle/service/hle_ipc.h": STUB,
            "common/common_types.h": (
                "#pragma once\n#include <cstdint>\n#include <memory>\n"
                "using u32 = std::uint32_t; using u64 = std::uint64_t;\n"
            ),
            "common/container/unordered_map.h": (
                "#pragma once\n#include <unordered_map>\n"
                "namespace Common { template <typename K, typename V> "
                "using unordered_map = std::unordered_map<K, V>; }\n"
            ),
            "core/hle/kernel/k_thread.h": "#pragma once\n",
            "common/logging.h": LOGGER,
            "trace.cpp": DRIVER,
        }.items():
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(contents)
        cls.binary = directory / "trace"
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("A C++20 compiler is required")
        compilation = subprocess.run(
            [
                compiler,
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(directory),
                "-I",
                str(ROOT / "src"),
                str(directory / "trace.cpp"),
                "-o",
                str(cls.binary),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        if compilation.returncode:
            raise RuntimeError(compilation.stderr)

    def run_trace(self, mode, enabled=None, thread=None):
        environment = os.environ.copy()
        for key, value in {
            "NXBOX_AM_FS_POLL_TRACE": enabled,
            "NXBOX_AM_FS_POLL_THREAD": thread,
        }.items():
            environment.pop(key, None)
            if value is not None:
                environment[key] = value
        subprocess.run([str(self.binary), mode], env=environment, check=True, timeout=10)

    def test_default_off_and_explicit_opt_out(self):
        self.run_trace("disabled")
        self.run_trace("disabled", "0")

    def test_repeated_calls_results_and_default_dbsz_thread(self):
        self.run_trace("enabled", "1")

    def test_thread_filter(self):
        self.run_trace("other", "1", "82")
        self.run_trace("enabled", "1", "0")
        self.run_trace("other", "1", "84")
        self.run_trace("enabled", "1", "invalid")
        self.run_trace("enabled", "1", "82trailing")

    def test_visible_log_limit_preserves_dispatch(self):
        self.run_trace("limit", "1")

    def test_all_am_fs_endpoints_use_adapter(self):
        count = 0
        for name in ("am", "filesystem"):
            for path in (ROOT / "src/core/hle/service" / name).rglob("*"):
                if path.suffix not in (".cpp", ".h") or path == HEADER:
                    continue
                source = path.read_text()
                self.assertNotIn("public ServiceFramework<", source, str(path))
                self.assertNotIn(": ServiceFramework{", source, str(path))
                count += source.count("public PollingServiceFramework<")
        self.assertGreaterEqual(count, 37)


if __name__ == "__main__":
    unittest.main()
