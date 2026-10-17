// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "network/packet.h"
#include "network/room_member.h"

namespace Network {

// Eden's version-1 room wire format; keep it compatible with desktop clients.
inline bool IsValidLdnPayload(LDNPacketType type, std::size_t size) {
    switch (type) {
    case LDNPacketType::Scan:
    case LDNPacketType::DestroyNetwork:
        return size == 0;
    case LDNPacketType::Connect:
    case LDNPacketType::Disconnect:
        return size == 0x40; // Switch NodeInfo
    case LDNPacketType::ScanResp:
    case LDNPacketType::SyncNetwork:
        return size == 0x480; // Switch NetworkInfo
    }
    return false;
}

inline Packet SerializeLdnPacket(const LDNPacket& ldn) {
    Packet packet;
    packet.Write(static_cast<u8>(IdLdnPacket));
    packet.Write(static_cast<u8>(ldn.type));
    packet.Write(ldn.local_ip);
    packet.Write(ldn.remote_ip);
    packet.Write(ldn.broadcast);
    packet.Write(ldn.data);
    return packet;
}

inline bool DeserializeLdnPacket(Packet& packet, LDNPacket& out) {
    LDNPacket candidate{};
    u8 message{}, type{}, broadcast{};
    packet.Read(message);
    packet.Read(type);
    packet.Read(candidate.local_ip);
    packet.Read(candidate.remote_ip);
    packet.Read(broadcast);
    packet.Read(candidate.data);
    candidate.type = static_cast<LDNPacketType>(type);
    candidate.broadcast = broadcast != 0;
    if (!packet || !packet.EndOfPacket() || message != IdLdnPacket || broadcast > 1 ||
        !IsValidLdnPayload(candidate.type, candidate.data.size())) {
        return false;
    }
    out = std::move(candidate);
    return true;
}

} // namespace Network
