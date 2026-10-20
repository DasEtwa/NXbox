# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production UDP trace accounting, including failed and empty sends."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BSD = ROOT / "src/core/hle/service/sockets/bsd.cpp"

DRIVER = r"""
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>
using s32 = int;
using u64 = unsigned long long;
enum class Errno { SUCCESS = 0, AGAIN = 11 };
namespace Network {
struct SockAddrIn { int ip; int portno; };
std::string IPv4AddressToString(int) { return "192.0.2.1"; }
}
std::vector<std::string> messages;
namespace fmt {
inline std::string format(std::string pattern) { return pattern; }
template <typename T, typename... Args>
std::string format(std::string pattern, const T& value, const Args&... args) {
    const auto pos = pattern.find("{}");
    const auto text = [&] {
        if constexpr (std::is_convertible_v<T, std::string>) return std::string(value);
        else return std::to_string(value);
    }();
    pattern.replace(pos, 2, text);
    return format(pattern, args...);
}
}
#define LOG_INFO(category, ...) messages.push_back(fmt::format(__VA_ARGS__))
struct TraceClock {
    static inline std::chrono::steady_clock::time_point tick{};
    static auto now() { return tick; }
};
TRACE
template <typename Trace>
void RecordFailedSend(const Network::SockAddrIn& peer) {
    if constexpr (requires { Trace::Record(2, peer, true, 0, Errno::AGAIN); }) {
        Trace::Record(2, peer, true, 0, Errno::AGAIN);
    } else {
        Trace::Record(2, peer, true, 0);
    }
}
int main() {
    const Network::SockAddrIn peer{0, 1234};
    // Failed sends must not be reported as successful packets, even when bytes == 0.
    RecordFailedSend<UdpPeerTrace>(peer);
    assert(messages.size() == 1);
    assert(messages.front().find("first=send-error") != std::string::npos);
    // A legitimate zero-byte UDP datagram is still one successful packet.
    UdpPeerTrace::Record(2, peer, true, 0);
    UdpPeerTrace::Record(2, peer, true, 692);
    UdpPeerTrace::Record(2, peer, false, 500);
    TraceClock::tick += std::chrono::seconds{5};
    UdpPeerTrace::Record(2, peer, false, 192);
    assert(messages.size() == 2);
    assert(messages.back().find("sent=2/692B recv=2/692B send_errors=1 last_errno=11") != std::string::npos);
    // Each interval resets all counters; a different descriptor has independent accounting.
    UdpPeerTrace::Record(3, peer, true, 16);
    TraceClock::tick += std::chrono::seconds{5};
    UdpPeerTrace::Record(2, peer, true, 84);
    assert(messages[messages.size() - 2].find("fd=2") != std::string::npos);
    assert(messages[messages.size() - 2].find("sent=1/84B recv=0/0B send_errors=0 last_errno=0") != std::string::npos);
    assert(messages.back().find("fd=3") != std::string::npos);
    assert(messages.back().find("sent=1/16B") != std::string::npos);
    // An interval containing only errors must still be emitted.
    TraceClock::tick += std::chrono::seconds{5};
    RecordFailedSend<UdpPeerTrace>(peer);
    assert(messages.back().find("sent=0/0B recv=0/0B send_errors=1 last_errno=11") != std::string::npos);
}
"""


class UdpPeerTraceTests(unittest.TestCase):
    def test_failed_send_is_not_counted_as_successful(self):
        source = BSD.read_text()
        start = source.index("class UdpPeerTrace {")
        end = source.index("\n};", start) + len("\n};")
        trace = source[start:end].replace("std::chrono::steady_clock", "TraceClock")
        with tempfile.TemporaryDirectory(prefix="nxbox-udp-trace-") as directory:
            driver = Path(directory) / "trace.cpp"
            binary = Path(directory) / "trace"
            driver.write_text(DRIVER.replace("TRACE", trace))
            build = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    str(driver),
                    "-o",
                    str(binary),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], check=False, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_send_path_passes_the_actual_socket_result(self):
        source = BSD.read_text()
        start = source.index("BSD_USA::SendToImpl(")
        end = source.index("\n}\n", start)
        self.assertIn("result.second", source[start:end].split("UdpPeerTrace::Record(")[1])


if __name__ == "__main__":
    unittest.main()
