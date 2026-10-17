// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "core/hle/service/nextendo/auth.h"

namespace Service::Nextendo {

inline std::string JsonString(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += digits[c >> 4];
            result += digits[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

// These Nintendo-looking issuer/JWKS strings are protocol identifiers only. The DNS hook
// redirects them before resolution; this function performs no network operation.
inline std::string IdTokenClaims(const Identity& identity, std::string_view version,
                                 std::int64_t now, std::string_view nonce) {
    if (identity.pid == 0 || identity.nex_token.empty() || nonce.empty() || now < 0)
        return {};
    constexpr std::string_view issuer =
        "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
    return "{\"iss\":" + JsonString(issuer) + ",\"aud\":\"ed9e2f05d286f7b8\",\"jku\":" +
           JsonString(std::string(issuer) + "/1.0.0/certificates") +
           ",\"sub\":" + JsonString(nonce) + ",\"jti\":" + JsonString(nonce) +
           ",\"di\":" + JsonString(nonce) + ",\"bs:did\":" + JsonString(nonce) +
           ",\"sn\":\"XAW10000000000\",\"hm\":true,\"nnex\":" + JsonString(identity.nex_token) +
           ",\"tv\":" + JsonString(version) + ",\"iat\":" + std::to_string(now) +
           ",\"nbf\":" + std::to_string(now) + ",\"exp\":" + std::to_string(now + 10800) + '}';
}

} // namespace Service::Nextendo
