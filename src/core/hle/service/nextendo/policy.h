// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Service::Nextendo {

enum class Destination { Native, Main, Nat, Blocked };

inline std::optional<std::string> NormalizeHost(std::string_view input) {
    if (input.empty() || input.size() > 254) {
        return std::nullopt;
    }
    std::string host(input);
    if (host.back() == '.') {
        host.pop_back();
    }
    if (host.empty() || host.size() > 253) {
        return std::nullopt;
    }
    std::size_t label = 0;
    for (char& c : host) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c == '.') {
            if (label == 0 || label > 63) {
                return std::nullopt;
            }
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            ++label;
        } else {
            return std::nullopt;
        }
    }
    return label == 0 || label > 63 ? std::nullopt : std::optional{host};
}

inline bool InDomain(std::string_view host, std::string_view domain) {
    return host == domain || (host.size() > domain.size() && host.ends_with(domain) &&
                              host[host.size() - domain.size() - 1] == '.');
}

inline bool IsNintendoHost(std::string_view host) {
    constexpr std::array domains = {"nintendo"
                                    ".net",
                                    "nintendo.com",
                                    "nintendowifi.net",
                                    "nintendo.co.jp",
                                    "nintendo.es",
                                    "nintendo-europe.com",
                                    "nintendo.com.hk",
                                    "nintendo.com.au",
                                    "nintendo.co.kr",
                                    "nintendo.co.uk",
                                    "nintendo.co.nz",
                                    "nintendo.co.za",
                                    "nintendo.jp",
                                    "nintendo.tw",
                                    "nintendo.at",
                                    "nintendo.be",
                                    "nintendo.dk",
                                    "nintendo.de",
                                    "nintendo.fi",
                                    "nintendo.fr",
                                    "nintendo.gr",
                                    "nintendo.hu",
                                    "nintendo.it",
                                    "nintendo.nl",
                                    "nintendo.no",
                                    "nintendo.pt",
                                    "nintendo.ru",
                                    "nintendo.ch",
                                    "nintendo.se",
                                    "nintendoswitch.com.cn",
                                    "nintendoswitch.com"};
    for (const auto domain : domains) {
        if (InDomain(host, domain)) {
            return true;
        }
    }
    return false;
}

// Ordered protocol facts observed in Ryujinx-Nextendo, not a port of its resolver.
inline Destination ClassifyHost(std::string_view input) {
    const auto normalized = NormalizeHost(input);
    if (!normalized) {
        return Destination::Blocked;
    }
    const std::string_view host = *normalized;
    if ((host.starts_with("nncs2-") && host.ends_with(".n.n.srv.nintendo"
                                                      ".net")) ||
        host == "g2b309e01-lp1.s.n.srv.nintendo"
                ".net" ||
        host == "g2df33d01-lp1.p.srv.nintendo"
                ".net") {
        return Destination::Nat;
    }
    for (const auto domain :
         {"nintendo"
          ".net",
          "nintendo.com", "nintendowifi.net", "nintendo.co.jp", "demonware.net"}) {
        if (InDomain(host, domain)) {
            return Destination::Main;
        }
    }
    return IsNintendoHost(host) ? Destination::Blocked : Destination::Native;
}

// Routing accepts only operator-provided numeric IPv4 addresses: no secondary DNS lookup
// can accidentally resolve a replacement name to Nintendo. Distinct NAT responders are required.
inline bool IsServerAddress(std::string_view address) {
    std::array<unsigned, 4> octets{};
    std::size_t offset = 0;
    for (auto& octet : octets) {
        const auto start = offset;
        while (offset < address.size() && address[offset] >= '0' && address[offset] <= '9') {
            octet = octet * 10 + static_cast<unsigned>(address[offset++] - '0');
            if (octet > 255 || offset - start > 3) {
                return false;
            }
        }
        if (start == offset || (offset - start > 1 && address[start] == '0')) {
            return false;
        }
        if (&octet != &octets.back()) {
            if (offset >= address.size() || address[offset++] != '.') {
                return false;
            }
        }
    }
    return offset == address.size() && octets[0] != 0 && octets[0] != 127 && octets[0] < 224 &&
           !(octets[0] == 169 && octets[1] == 254);
}

struct Config {
    bool enabled = false;
    std::string server_address;
    std::string nat_address;
    std::string client_id;

    bool RoutesReady() const {
        return IsServerAddress(server_address) && IsServerAddress(nat_address) &&
               server_address != nat_address;
    }
};

struct Route {
    Destination destination;
    std::string address;
};

inline Route ResolveHost(const Config& config, std::string_view host) {
    if (!config.enabled) {
        return {Destination::Native, std::string(host)};
    }
    const auto destination = ClassifyHost(host);
    if (destination == Destination::Main || destination == Destination::Nat) {
        if (!config.RoutesReady()) {
            return {Destination::Blocked, {}};
        }
        return {destination,
                destination == Destination::Nat ? config.nat_address : config.server_address};
    }
    return {destination, destination == Destination::Native ? std::string(host) : std::string{}};
}

} // namespace Service::Nextendo
