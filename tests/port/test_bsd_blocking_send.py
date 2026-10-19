# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""A blocking guest socket must not see EAGAIN from a send (bsd.cpp SendAsGuestExpects)."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BSD = ROOT / "src/core/hle/service/sockets/bsd.cpp"


def helper(source):
    start = source.index("template <typename SendOnce>")
    end = source.index("} // namespace", start)
    return source[start:end]


DRIVER = r"""
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
using s32 = std::int32_t;
using u32 = std::uint32_t;
namespace Network { constexpr u32 FLAG_O_NONBLOCK = 0x800; }
enum class Errno { SUCCESS, AGAIN };
HELPER
int main() {
    // EAGAIN twice (the recv thread's non-blocking window), then the send goes through.
    int calls = 0;
    auto flaky = [&] { return ++calls < 3 ? std::pair{-1, Errno::AGAIN} : std::pair{340, Errno::SUCCESS}; };
    assert((SendAsGuestExpects(0, flaky) == std::pair{340, Errno::SUCCESS}) && calls == 3);
    // A non-blocking guest socket gets EAGAIN back at once.
    calls = 0;
    assert(SendAsGuestExpects(static_cast<s32>(Network::FLAG_O_NONBLOCK), flaky).second == Errno::AGAIN && calls == 1);
    // A host socket that never takes the data gives up after the bounded wait.
    calls = 0;
    auto stuck = [&] { ++calls; return std::pair{-1, Errno::AGAIN}; };
    assert(SendAsGuestExpects(0, stuck).second == Errno::AGAIN && calls == 51);
}
"""


class BlockingSendTests(unittest.TestCase):
    def test_both_send_paths_use_the_helper(self):
        source = BSD.read_text()
        for name in ("BSD_USA::SendImpl(", "BSD_USA::SendToImpl("):
            body = source[source.index(name) :]
            body = body[: body.index("\n}\n")]
            self.assertIn("SendAsGuestExpects(descriptor.flags", body)

    def test_retry_semantics(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-bsd-send-") as temp:
            source = Path(temp) / "send.cpp"
            binary = Path(temp) / "send"
            source.write_text(DRIVER.replace("HELPER", helper(BSD.read_text())))
            subprocess.run(
                ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
