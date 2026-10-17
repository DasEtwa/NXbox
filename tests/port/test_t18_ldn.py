# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the production room codec and LDN state machine in /tmp."""

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
#include <future>
#include <iostream>
#include <thread>
#include "network/ldn_packet.h"
#include "network/proxy_packet.h"
#include "eden_uwp/multiplayer_strings.h"
#include "core/hle/service/ldn/lan_discovery.h"
namespace EdenXbox::Ui {
Language language = Language::English;
Language CurrentLanguage() { return language; }
}
using namespace Network;
using namespace Service::LDN;
namespace Network {
class RoomMember::RoomMemberImpl {};
RoomMember::RoomMember() = default;
RoomMember::~RoomMember() = default;
RoomMember::State RoomMember::GetState() const { return State::Joined; }
bool RoomMember::IsConnected() const { return true; }
const IPv4Address& RoomMember::GetFakeIpAddress() const {
    static IPv4Address ip{10, 0, 0, 2}; return ip;
}
IPv4Address RoomMember::GetFakeIpAddressSnapshot() const { return GetFakeIpAddress(); }
std::atomic<int> sent_connect{};
std::atomic<int> sent_scan{};
void RoomMember::SendLdnPacket(const LDNPacket& p) {
    if (p.type == LDNPacketType::Connect) ++sent_connect;
    if (p.type == LDNPacketType::Scan) ++sent_scan;
}
std::weak_ptr<RoomMember> GetRoomMember() {
    static auto member = std::make_shared<RoomMember>(); return member;
}
}
[[noreturn]] void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
LDNPacket Wire(LDNPacketType type, const void* data = nullptr, size_t size = 0) {
    LDNPacket p{}; p.type = type; p.local_ip = {10, 0, 0, 1};
    p.remote_ip = {10, 0, 0, 2}; p.data.resize(size);
    if (size) std::memcpy(p.data.data(), data, size);
    return p;
}
int main() {
    int cases = 0;
    EdenXbox::Ui::language = EdenXbox::Ui::Language::Portuguese;
    assert(std::wstring(EdenXbox::Ui::Multiplayer::Tr(EdenXbox::Ui::Multiplayer::Text::Nickname)) == L"Seu apelido"); ++cases;
    EdenXbox::Ui::language = EdenXbox::Ui::Language::English;
    assert(std::wstring(EdenXbox::Ui::Multiplayer::Tr(EdenXbox::Ui::Multiplayer::Text::Nickname)) == L"Your nickname"); ++cases;
    for (u8 type = 0; type < 6; ++type) {
        size_t size = (type == 1 || type == 3) ? 0x480 : (type == 2 || type == 4) ? 0x40 : 0;
        std::vector<u8> data(size, 0xA5);
        auto p = Wire(static_cast<LDNPacketType>(type), data.data(), size);
        p.broadcast = type == 0;
        auto serialized = SerializeLdnPacket(p);
        const auto* bytes = static_cast<const u8*>(serialized.GetData());
        assert(bytes[0] == IdLdnPacket && bytes[1] == type);
        assert(bytes[11] == (size >> 24) && bytes[14] == (size & 0xff));
        LDNPacket decoded{};
        assert(DeserializeLdnPacket(serialized, decoded));
        assert(decoded.type == p.type && decoded.data == p.data && decoded.local_ip == p.local_ip);
        ++cases;
        for (size_t cut = 0; cut < serialized.GetDataSize(); ++cut) {
            Packet truncated; truncated.Append(bytes, cut);
            assert(!DeserializeLdnPacket(truncated, decoded));
        }
        ++cases;
    }
    for (int mutation : {0, 1, 10, 11, 14, 15}) {
        auto packet = SerializeLdnPacket(Wire(LDNPacketType::Scan));
        auto* bytes = const_cast<u8*>(static_cast<const u8*>(packet.GetData()));
        if (mutation == 15) packet.Write(u8{0});
        else bytes[mutation] = 255;
        LDNPacket decoded{};
        assert(!DeserializeLdnPacket(packet, decoded)); ++cases;
    }
    {
        Packet p; p.Write(u32{0xffffffff}); std::vector<u8> out{1};
        p.Read(out); assert(!p && out.empty()); ++cases;
        Packet skip; skip.IgnoreBytes(1); assert(!skip); ++cases;
        Packet numeric; numeric.Write(u64{0x0102030405060708}); numeric.Write(std::string("room"));
        u64 value{}; std::string name; numeric.Read(value); numeric.Read(name);
        assert(numeric && value == 0x0102030405060708 && name == "room"); ++cases;
    }
    ProxyPacket proxy{}; proxy.protocol = Protocol::UDP;
    proxy.local_endpoint.family = Domain::INET; proxy.local_endpoint.portno = 0x1234;
    proxy.remote_endpoint.family = Domain::INET; proxy.remote_endpoint.portno = 0x5678;
    proxy.data = {1,2,3,4};
    auto proxy_wire = SerializeProxyPacket(proxy); ProxyPacket proxy_decoded{};
    assert(DeserializeProxyPacket(proxy_wire, proxy_decoded) && proxy_decoded.data == proxy.data &&
           proxy_decoded.local_endpoint.portno == 0x1234 && proxy_decoded.remote_endpoint.portno == 0x5678); ++cases;
    for (size_t cut = 0; cut < proxy_wire.GetDataSize(); ++cut) {
        Packet truncated; truncated.Append(proxy_wire.GetData(), cut);
        assert(!DeserializeProxyPacket(truncated, proxy_decoded));
    }
    ++cases;
    LANDiscovery host; assert(host.Initialize().IsSuccess());
    UserConfig user{}; NetworkConfig config{}; SecurityConfig security{};
    config.node_count_max = 2;
    assert(host.CreateNetwork(security, user, config).IsError()); ++cases;
    assert(host.OpenAccessPoint().IsSuccess());
    config.node_count_max = 9;
    assert(host.CreateNetwork(security, user, config).IsError()); ++cases;
    config.node_count_max = 2;
    assert(host.CreateNetwork(security, user, config).IsSuccess());
    NodeInfo client{}; client.mac_address.raw = {2,0,10,0,0,1};
    auto connect = Wire(LDNPacketType::Connect, &client, sizeof(client));
    host.ReceivePacket(connect);
    NetworkInfo info{}; assert(host.GetNetworkInfo(info).IsSuccess());
    assert(info.ldn.node_count == 2); ++cases;
    host.ReceivePacket(connect); host.GetNetworkInfo(info); assert(info.ldn.node_count == 2); ++cases;
    auto other = connect; other.local_ip[3] = 3;
    host.ReceivePacket(other); host.GetNetworkInfo(info); assert(info.ldn.node_count == 2); ++cases;
    auto disconnect = Wire(LDNPacketType::Disconnect, &client, sizeof(client));
    disconnect.local_ip[3] = 3;
    host.ReceivePacket(disconnect); host.GetNetworkInfo(info); assert(info.ldn.node_count == 2); ++cases;
    disconnect.local_ip = connect.local_ip;
    host.ReceivePacket(disconnect); host.GetNetworkInfo(info); assert(info.ldn.node_count == 1); ++cases;
    for (u8 type = 0; type < 6; ++type) {
        auto malformed = Wire(static_cast<LDNPacketType>(type));
        malformed.data.resize(1); host.ReceivePacket(malformed);
    }
    host.GetNetworkInfo(info); assert(info.ldn.node_count == 1); ++cases;
    host.ReceivePacket(connect); host.UpdateRoomMembers({});
    host.GetNetworkInfo(info); assert(info.ldn.node_count == 1); ++cases;
    host.OnRoomDisconnected(); assert(host.GetState() == State::AccessPointOpened); ++cases;
    assert(host.Finalize().IsSuccess()); assert(host.GetState() == State::None); ++cases;
    LANDiscovery station; station.Initialize(); station.OpenStation();
    info.ldn.node_count = 1; info.ldn.node_count_max = 2;
    info.ldn.nodes[0].ipv4_address = {1,0,0,10};
    info.ldn.nodes[1].ipv4_address = {2,0,0,10}; info.ldn.nodes[1].is_connected = 0;
    sent_connect = 0;
    auto future = std::async(std::launch::async, [&] { return station.Connect(info, user, 1); });
    while (!sent_connect) std::this_thread::yield();
    auto synced_info = info; synced_info.ldn.node_count = 2; synced_info.ldn.nodes[1].is_connected = 1;
    auto sync = Wire(LDNPacketType::SyncNetwork, &synced_info, sizeof(synced_info));
    sync.local_ip[3] = 3; station.ReceivePacket(sync);
    assert(station.GetState() == State::StationOpened); ++cases;
    sync.local_ip[3] = 1; station.ReceivePacket(sync);
    assert(future.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready);
    assert(future.get().IsSuccess()); assert(station.GetState() == State::StationConnected); ++cases;
    auto destroy = Wire(LDNPacketType::DestroyNetwork);
    destroy.local_ip[3] = 3; station.ReceivePacket(destroy);
    assert(station.GetState() == State::StationConnected); ++cases;
    destroy.local_ip[3] = 1; station.ReceivePacket(destroy);
    assert(station.GetState() == State::StationOpened);
    assert(station.GetDisconnectReason() == DisconnectReason::DestroyedByUser); ++cases;
    s16 empty_count = 99;
    assert(station.Scan({}, empty_count, {}).IsError() && empty_count == 0); ++cases;
    sent_scan = 0;
    std::array<NetworkInfo, 2> networks{}; s16 count = 99; ScanFilter filter{};
    auto scan = std::async(std::launch::async, [&] { return station.Scan(networks, count, filter); });
    while (!sent_scan) std::this_thread::yield();
    station.ReceivePacket(Wire(LDNPacketType::ScanResp, &info, sizeof(info)));
    assert(scan.get().IsSuccess() && count == 1); ++cases;
    info.ldn.advertise_data_size = AdvertiseDataSizeMax + 1;
    station.ReceivePacket(Wire(LDNPacketType::SyncNetwork, &info, sizeof(info)));
    assert(station.GetState() == State::StationOpened); ++cases;
    info.ldn.advertise_data_size = 0;
    sent_connect = 0;
    auto cancelled = std::async(std::launch::async, [&] { return station.Connect(info, user, 1); });
    while (!sent_connect) std::this_thread::yield();
    station.Finalize();
    assert(cancelled.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready);
    assert(cancelled.get().IsError() && station.GetState() == State::None); ++cases;
    station.Initialize(); station.OpenStation();
    auto timed_out = station.Connect(info, user, 1);
    assert(timed_out.IsError() && station.GetState() == State::StationOpened); ++cases;
    station.Finalize();
    LANDiscovery policies; policies.Initialize(); policies.OpenAccessPoint();
    policies.CreateNetwork(security, user, config);
    assert(policies.SetStationAcceptPolicy(AcceptPolicy::RejectAll).IsSuccess());
    policies.ReceivePacket(connect); policies.GetNetworkInfo(info); assert(info.ldn.node_count == 1); ++cases;
    policies.SetStationAcceptPolicy(AcceptPolicy::WhiteList);
    policies.ReceivePacket(connect); policies.GetNetworkInfo(info); assert(info.ldn.node_count == 1); ++cases;
    MacAddress allowed{}; allowed.raw = {2,0,10,0,0,1};
    assert(policies.AddAcceptFilterEntry(allowed).IsSuccess());
    policies.ReceivePacket(connect); policies.GetNetworkInfo(info); assert(info.ldn.node_count == 2); ++cases;
    assert(policies.Reject({1,0,0,10}).IsSuccess()); policies.GetNetworkInfo(info);
    assert(info.ldn.node_count == 1); ++cases;
    policies.SetStationAcceptPolicy(AcceptPolicy::BlackList);
    policies.ReceivePacket(connect); policies.GetNetworkInfo(info); assert(info.ldn.node_count == 1); ++cases;
    policies.ClearAcceptFilter(); policies.ReceivePacket(connect); policies.GetNetworkInfo(info);
    assert(info.ldn.node_count == 2); ++cases;
    assert(policies.SetStationAcceptPolicy(static_cast<AcceptPolicy>(4)).IsError()); ++cases;
    policies.Finalize();
    LANDiscovery private_host; private_host.Initialize(); private_host.OpenAccessPoint();
    SecurityParameter parameter{}; parameter.session_id = {123,456}; parameter.data.fill(0xCD);
    assert(private_host.CreateNetwork(security, user, config, &parameter).IsSuccess());
    private_host.GetNetworkInfo(info);
    assert(info.network_id.session_id == parameter.session_id && info.ldn.security_parameter == parameter.data); ++cases;
    info.ldn.nodes[0].ipv4_address = {1,0,0,10};
    info.ldn.nodes[1].ipv4_address = {2,0,0,10}; info.ldn.nodes[1].is_connected = 0;
    station.Initialize(); station.OpenStation();
    ConnectNetworkPrivateData private_data{};
    private_data.security_parameter = parameter; private_data.network_config = config;
    sent_scan = 0; sent_connect = 0;
    auto private_connection = std::async(std::launch::async, [&] { return station.ConnectPrivate(private_data); });
    while (!sent_scan) std::this_thread::yield();
    station.ReceivePacket(Wire(LDNPacketType::ScanResp, &info, sizeof(info)));
    while (!sent_connect) std::this_thread::yield();
    synced_info = info; synced_info.ldn.node_count = 2; synced_info.ldn.nodes[1].is_connected = 1;
    station.ReceivePacket(Wire(LDNPacketType::SyncNetwork, &synced_info, sizeof(synced_info)));
    assert(private_connection.get().IsSuccess()); ++cases;
    station.OnRoomDisconnected(); assert(station.GetState() == State::StationOpened); ++cases;
    private_data.local_communication_version = -1;
    assert(station.ConnectPrivate(private_data).IsError()); ++cases;
    station.Finalize(); private_host.Finalize();
    station.Initialize(); station.OpenStation();
    sent_connect = 0;
    auto rejected = std::async(std::launch::async, [&] { return station.Connect(info, user, 1); });
    while (!sent_connect) std::this_thread::yield();
    station.ReceivePacket(Wire(LDNPacketType::DestroyNetwork));
    assert(rejected.get().IsError());
    assert(station.GetDisconnectReason() == DisconnectReason::Rejected); ++cases;
    station.Finalize();
    std::cout << "T18: " << cases << " native cases passed; 6/6 LDN wire types exercised\n";
}
"""


class LdnTests(unittest.TestCase):
    def test_production_packets_and_state(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-t18-", dir="/tmp") as directory:
            directory = Path(directory)
            (directory / "common").mkdir()
            (directory / "common/logging.h").write_text(
                "#pragma once\n#define LOG_INFO(...) ((void)0)\n"
                "#define LOG_ERROR(...) ((void)0)\n#define LOG_CRITICAL(...) ((void)0)\n"
            )
            source = directory / "test.cpp"
            source.write_text(HARNESS)
            binary = directory / "test"
            include = Path("/opt/homebrew/include")
            flags = ["-std=c++20", "-pthread", "-I", str(directory), "-I", str(ROOT / "src")]
            if include.exists():
                flags += ["-I", str(include)]
            # Optional LLVM coverage writes only outside the repository.
            if os.environ.get("T18_SANITIZERS"):
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            coverage = os.environ.get("T18_COVERAGE_DIR")
            if coverage:
                flags += ["-fprofile-instr-generate", "-fcoverage-mapping"]
            subprocess.run(
                [
                    shutil.which("clang++") or "c++",
                    *flags,
                    str(source),
                    str(ROOT / "src/network/packet.cpp"),
                    str(ROOT / "src/core/hle/service/ldn/lan_discovery.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                text=True,
            )
            env = dict(os.environ)
            if coverage:
                Path(coverage).mkdir(parents=True, exist_ok=True)
                env["LLVM_PROFILE_FILE"] = str(Path(coverage) / "ldn.profraw")
                shutil.copy2(binary, Path(coverage) / "ldn-tests")
            subprocess.run([str(binary)], check=True, timeout=20, env=env)


if __name__ == "__main__":
    unittest.main()
