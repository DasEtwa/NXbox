# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional real ENet localhost interoperability test (no emulator or game data)."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r"""
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>
#include "network/network.h"
using namespace Network;
using namespace std::chrono_literals;
void AssertFailSoftImpl() { std::abort(); }
[[noreturn]] void AssertFatalImpl() { std::abort(); }
bool Joined(const RoomMember& m) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (m.GetState() == RoomMember::State::Joining && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    return m.GetState() == RoomMember::State::Joined || m.GetState() == RoomMember::State::Moderator;
}
int main() {
    assert(Init());
    auto room = GetRoom().lock();
    assert(room->Create("T18 localhost", "", "127.0.0.1", 0, "secret", 2));
    // ENet resolves port 0 to an ephemeral port, exported by Room::Create.
    const auto port = room->GetRoomInformation().port;
    assert(port != 0);
    RoomMember first, second, third;
    first.Join("First", "127.0.0.1", port, 0, NoPreferredIP, "secret"); assert(Joined(first));
    second.Join("Second", "127.0.0.1", port, 0, NoPreferredIP, "secret"); assert(Joined(second));
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (first.GetMemberInformationSnapshot().size() != 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    assert(first.GetMemberInformationSnapshot().size() == 2);
    std::atomic<int> received{};
    auto callback = first.BindOnLdnPacketReceived([&](const LDNPacket& p) {
        assert(p.type == LDNPacketType::Scan && p.broadcast && p.data.empty());
        assert(p.local_ip == second.GetFakeIpAddressSnapshot()); ++received;
    });
    LDNPacket packet{}; packet.type = LDNPacketType::Scan; packet.broadcast = true;
    packet.local_ip = {1,2,3,4}; second.SendLdnPacket(packet);
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!received && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    assert(received == 1);
    std::atomic<int> proxy_received{};
    auto proxy_callback = first.BindOnProxyPacketReceived([&](const ProxyPacket& p) {
        assert(p.protocol == Protocol::UDP && !p.broadcast);
        assert(p.local_endpoint.ip == second.GetFakeIpAddressSnapshot());
        assert(p.remote_endpoint.ip == first.GetFakeIpAddressSnapshot());
        assert(p.local_endpoint.portno == 12340 && p.remote_endpoint.portno == 4500);
        assert(p.data == std::vector<u8>({1,2,3,4})); ++proxy_received;
    });
    ProxyPacket datagram{};
    datagram.local_endpoint.family = Domain::INET;
    datagram.local_endpoint.ip = {1,2,3,4};
    datagram.local_endpoint.portno = 12340;
    datagram.remote_endpoint.family = Domain::INET;
    datagram.remote_endpoint.ip = first.GetFakeIpAddressSnapshot();
    datagram.remote_endpoint.portno = 4500;
    datagram.protocol = Protocol::UDP; datagram.data = {1,2,3,4};
    second.SendProxyPacket(datagram);
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!proxy_received && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    assert(proxy_received == 1);
    first.Unbind(proxy_callback);
    std::atomic<RoomMember::Error> error{RoomMember::Error::UnknownError};
    auto errors = third.BindOnError([&](const auto& value) { error = value; });
    third.Join("Third", "127.0.0.1", port, 0, NoPreferredIP, "secret");
    assert(!Joined(third)); assert(error == RoomMember::Error::RoomIsFull); third.Leave();
    second.Leave();
    third.Join("Third", "127.0.0.1", port, 0, NoPreferredIP, "wrong");
    assert(!Joined(third)); assert(error == RoomMember::Error::WrongPassword); third.Leave();
    third.Join("First", "127.0.0.1", port, 0, NoPreferredIP, "secret");
    assert(!Joined(third)); assert(error == RoomMember::Error::NameCollision); third.Leave();
    third.Join("Third", "127.0.0.1", port, 0, NoPreferredIP, "secret"); assert(Joined(third));
    first.Unbind(callback); third.Unbind(errors);
    first.Leave(); third.Leave(); room->Destroy(); Shutdown();
    std::puts("T18: 9 real ENet room scenarios passed");
}
"""


class RoomTests(unittest.TestCase):
    @unittest.skipUnless(
        os.environ.get("T18_ENET_SOURCE") and os.environ.get("T18_ENET_LIBRARY"),
        "Set T18_ENET_SOURCE and T18_ENET_LIBRARY for real ENet socket tests",
    )
    def test_real_room_transport_and_state(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-t18-room-", dir="/tmp") as directory:
            directory = Path(directory)
            (directory / "common").mkdir()
            (directory / "common/logging.h").write_text(
                "#pragma once\n#define FMT_HEADER_ONLY\n#include <fmt/format.h>\n"
                + "\n".join(
                    f"#define LOG_{level}(...) ((void)0)"
                    for level in ("INFO", "ERROR", "DEBUG", "CRITICAL")
                )
            )
            source = directory / "test.cpp"
            source.write_text(HARNESS)
            binary = directory / "test"
            flags = [
                "-std=c++20",
                "-pthread",
                "-I",
                str(directory),
                "-I",
                str(ROOT / "src"),
                "-I",
                str(Path(os.environ["T18_ENET_SOURCE"]) / "include"),
            ]
            if Path("/opt/homebrew/include").exists():
                flags += ["-I", "/opt/homebrew/include"]
            if os.environ.get("T18_SANITIZERS"):
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            coverage = os.environ.get("T18_COVERAGE_DIR")
            if coverage:
                flags += ["-fprofile-instr-generate", "-fcoverage-mapping"]
            sources = [
                ROOT / "src/network" / name
                for name in (
                    "network.cpp",
                    "room.cpp",
                    "room_member.cpp",
                    "packet.cpp",
                    "verify_user.cpp",
                )
            ]
            subprocess.run(
                [
                    shutil.which("clang++") or "c++",
                    *flags,
                    str(source),
                    *map(str, sources),
                    os.environ["T18_ENET_LIBRARY"],
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            env = dict(os.environ)
            if coverage:
                Path(coverage).mkdir(parents=True, exist_ok=True)
                env["LLVM_PROFILE_FILE"] = str(Path(coverage) / "room.profraw")
                shutil.copy2(binary, Path(coverage) / "room-tests")
            subprocess.run([str(binary)], check=True, timeout=30, env=env)


if __name__ == "__main__":
    unittest.main()
