// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "network/packet.h"
#include "network/room_member.h"
namespace Network {
inline Packet SerializeProxyPacket(const ProxyPacket& proxy) {
    Packet packet;
    packet.Write(static_cast<u8>(IdProxyPacket));
    packet.Write(static_cast<u8>(proxy.local_endpoint.family));
    packet.Write(proxy.local_endpoint.ip);
    packet.Write(proxy.local_endpoint.portno);
    packet.Write(static_cast<u8>(proxy.remote_endpoint.family));
    packet.Write(proxy.remote_endpoint.ip);
    packet.Write(proxy.remote_endpoint.portno);
    packet.Write(static_cast<u8>(proxy.protocol));
    packet.Write(proxy.broadcast);
    packet.Write(proxy.data);
    return packet;
}
inline bool DeserializeProxyPacket(Packet& packet, ProxyPacket& out) {
    ProxyPacket candidate{};
    u8 message{}, local_family{}, remote_family{}, protocol{}, broadcast{};
    packet.Read(message);
    packet.Read(local_family);
    packet.Read(candidate.local_endpoint.ip);
    packet.Read(candidate.local_endpoint.portno);
    packet.Read(remote_family);
    packet.Read(candidate.remote_endpoint.ip);
    packet.Read(candidate.remote_endpoint.portno);
    packet.Read(protocol);
    packet.Read(broadcast);
    packet.Read(candidate.data);
    if (!packet || !packet.EndOfPacket() || message != IdProxyPacket || broadcast > 1 ||
        local_family > static_cast<u8>(Domain::INET) ||
        remote_family > static_cast<u8>(Domain::INET)) {
        return false;
    }
    candidate.local_endpoint.family = static_cast<Domain>(local_family);
    candidate.remote_endpoint.family = static_cast<Domain>(remote_family);
    candidate.protocol = static_cast<Protocol>(protocol);
    candidate.broadcast = broadcast != 0;
    out = std::move(candidate);
    return true;
}
} // namespace Network
