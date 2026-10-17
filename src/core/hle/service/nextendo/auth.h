// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Service::Nextendo {

inline constexpr std::string_view ApiOrigin = "https://nextendo.network";
inline constexpr std::string_view CallbackUri = "http://127.0.0.1:17822/callback";

inline std::string UrlEncode(std::string_view input) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : input) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += digits[c >> 4];
            result += digits[c & 15];
        }
    }
    return result;
}

inline std::optional<std::string> UrlDecode(std::string_view input) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    std::string value;
    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size() || hex(input[i + 1]) < 0 || hex(input[i + 2]) < 0) {
                return std::nullopt;
            }
            c = static_cast<unsigned char>((hex(input[i + 1]) << 4) | hex(input[i + 2]));
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c < 32 || c == 127) {
            return std::nullopt;
        }
        value += static_cast<char>(c);
    }
    return value;
}

struct Identity {
    std::uint64_t pid = 0;
    std::string username;
    std::string friend_code;
    std::string nex_token;
    std::string profile_id;
};

inline bool SameSession(const Identity& current, const Identity& expected) {
    return current.pid != 0 && !current.nex_token.empty() && current.pid == expected.pid &&
           current.profile_id == expected.profile_id && current.nex_token == expected.nex_token;
}

struct HttpRequest {
    std::string method;
    std::string url;
    std::string content_type;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct HttpResponse {
    unsigned status = 0;
    Identity identity;
};

using Transport = std::function<HttpResponse(const HttpRequest&)>;
using SaveIdentity = std::function<bool(const Identity&)>;
enum class AuthResult {
    Ready,
    Pending,
    Cancelled,
    Expired,
    InvalidCallback,
    Rejected,
    StorageFailed
};

// No passwords, client secrets, device credentials, refresh guesses, or invented endpoints.
// HTTP is injected so the native adapter and the mock execute the same request policy.
class Authorization {
public:
    using Clock = std::chrono::steady_clock;
    Authorization(std::string client_id, std::string verifier, std::string challenge,
                  std::string state, Clock::time_point now)
        : client_id_(std::move(client_id)), verifier_(std::move(verifier)),
          challenge_(std::move(challenge)), state_(std::move(state)),
          deadline_(now + std::chrono::minutes(5)) {}

    std::string AuthorizeUrl() const {
        if (client_id_.empty() || verifier_.size() < 43 || verifier_.size() > 128 ||
            challenge_.size() != 43 || state_.size() < 32) {
            return {};
        }
        return std::string(ApiOrigin) +
               "/api/oauth/authorize?response_type=code&client_id=" + UrlEncode(client_id_) +
               "&redirect_uri=" + UrlEncode(CallbackUri) +
               "&scope=identity+friends+presence&app=nxbox&state=" + UrlEncode(state_) +
               "&code_challenge=" + UrlEncode(challenge_) + "&code_challenge_method=S256";
    }

    void Cancel() {
        result_ = AuthResult::Cancelled;
        verifier_.clear();
        state_.clear();
    }

    AuthResult Complete(std::string_view callback, Clock::time_point now,
                        std::string_view profile_id, const Transport& http,
                        const SaveIdentity& save) {
        if (result_ != AuthResult::Pending) {
            return result_;
        }
        if (now >= deadline_) {
            Cancel();
            return result_ = AuthResult::Expired;
        }
        const std::string prefix = std::string(CallbackUri) + '?';
        if (AuthorizeUrl().empty() || !callback.starts_with(prefix) || callback.size() > 4096 ||
            callback.find('#') != std::string_view::npos || profile_id.empty()) {
            return AuthResult::InvalidCallback;
        }
        std::map<std::string, std::string> fields;
        auto query = callback.substr(prefix.size());
        while (!query.empty()) {
            const auto end = query.find('&');
            const auto pair = query.substr(0, end);
            const auto equal = pair.find('=');
            if (equal == std::string_view::npos)
                return AuthResult::InvalidCallback;
            const auto key = UrlDecode(pair.substr(0, equal));
            const auto value = UrlDecode(pair.substr(equal + 1));
            if (!key || !value || !fields.emplace(*key, *value).second) {
                return AuthResult::InvalidCallback;
            }
            if (end == std::string_view::npos)
                break;
            query.remove_prefix(end + 1);
        }
        if (fields["state"] != state_) {
            return AuthResult::InvalidCallback;
        }
        if (fields.contains("error")) {
            Cancel();
            return result_ = AuthResult::Rejected;
        }
        if (fields["code"].empty())
            return AuthResult::InvalidCallback;
        HttpRequest request{"POST",
                            std::string(ApiOrigin) + "/api/oauth/token",
                            "application/x-www-form-urlencoded",
                            "grant_type=authorization_code&code=" + UrlEncode(fields["code"]) +
                                "&client_id=" + UrlEncode(client_id_) + "&redirect_uri=" +
                                UrlEncode(CallbackUri) + "&code_verifier=" + UrlEncode(verifier_),
                            {}};
        // Consume once even if the transport fails. A code cannot safely be replayed.
        Cancel();
        result_ = AuthResult::Rejected;
        try {
            auto response = http(request);
            if (response.status != 200 || response.identity.pid == 0 ||
                response.identity.nex_token.empty() || response.identity.nex_token.size() > 16384 ||
                response.identity.nex_token.find_first_of("\r\n") != std::string::npos) {
                return result_;
            }
            response.identity.profile_id = profile_id;
            return result_ =
                       save(response.identity) ? AuthResult::Ready : AuthResult::StorageFailed;
        } catch (...) {
            return result_;
        }
    }

private:
    std::string client_id_, verifier_, challenge_, state_;
    Clock::time_point deadline_;
    AuthResult result_ = AuthResult::Pending;
};

inline HttpRequest SocialRequest(std::string_view path, const Identity& identity,
                                 std::string_view client_id) {
    if (identity.pid == 0 || identity.nex_token.empty() ||
        identity.nex_token.find_first_of("\r\n") != std::string::npos ||
        client_id.find_first_of("\r\n") != std::string_view::npos ||
        (path != "/api/friends" && path != "/api/online-status" && path != "/api/presence" &&
         path != "/api/nex-session")) {
        return {};
    }
    HttpRequest request{path == "/api/presence" || path == "/api/nex-session" ? "POST" : "GET",
                        std::string(ApiOrigin) + std::string(path),
                        "application/json",
                        {},
                        {{"Authorization", "Bearer " + identity.nex_token}}};
    if (!client_id.empty())
        request.headers["X-Nextendo-Client-Id"] = client_id;
    return request;
}

} // namespace Service::Nextendo
