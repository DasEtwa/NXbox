// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/nextendo_online.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <windows.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/settings.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/nextendo/crypto.h"
#include "core/hle/service/nextendo/token.h"
#include "eden_uwp/await_bounded.h"

namespace EdenXbox::NextendoOnline {
namespace {
namespace nx = Service::Nextendo;
namespace json = winrt::Windows::Data::Json;
namespace http = winrt::Windows::Web::Http;
namespace credentials = winrt::Windows::Security::Credentials;

constexpr wchar_t AccountResource[] = L"NXbox.Nextendo.Account";
constexpr wchar_t KeyResource[] = L"NXbox.Nextendo.SigningKey";
constexpr wchar_t EnabledKey[] = L"nextendo_enabled";
std::mutex account_mutex;
std::recursive_mutex session_mutex;
bool account_verified = false;
// ponytail: fixed 60 s pause after a failed refresh; back off further if the server asks.
std::chrono::steady_clock::time_point retry_after;
nx::Identity account;
std::string profile_id;
std::shared_ptr<nx::IdTokenSigner> signer;
std::jthread social_worker;

nx::TokenProvider Provider() {
    const auto key = signer;
    if (!key)
        return {};
    return [key](const nx::Identity& identity, std::uint64_t, std::string_view version) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        try {
            const auto claims = nx::IdTokenClaims(identity, version, now, nx::RandomHex(16));
            return claims.empty() ? std::string{} : key->Sign(claims);
        } catch (...) {
            return std::string{};
        }
    };
}

// Certificate checks stay enabled. Never follow an HTTP redirect carrying account credentials.
std::optional<http::HttpResponseMessage> Send(const nx::HttpRequest& request) {
    if (!request.url.starts_with(std::string(nx::ApiOrigin) + "/api/") ||
        request.url.find_first_of("\r\n") != std::string::npos)
        return std::nullopt;
    http::Filters::HttpBaseProtocolFilter filter;
    filter.AllowAutoRedirect(false);
    http::HttpClient client(filter);
    http::HttpRequestMessage message(
        http::HttpMethod(winrt::to_hstring(request.method)),
        winrt::Windows::Foundation::Uri(winrt::to_hstring(request.url)));
    for (const auto& [name, value] : request.headers) {
        message.Headers().Append(winrt::to_hstring(name), winrt::to_hstring(value));
    }
    if (request.method == "POST") {
        message.Content(
            http::HttpStringContent(winrt::to_hstring(request.body),
                                    winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8,
                                    winrt::to_hstring(request.content_type)));
    }
    return AwaitBounded(client.SendRequestAsync(message), std::chrono::seconds(15));
}

std::optional<json::JsonObject> ReadJson(const http::HttpResponseMessage& response) {
    const auto text =
        AwaitBounded(response.Content().ReadAsStringAsync(), std::chrono::seconds(15));
    if (!text || text->size() > 1024 * 1024)
        return std::nullopt;
    json::JsonObject object;
    if (!json::JsonObject::TryParse(*text, object))
        return std::nullopt;
    return object;
}

std::int64_t ReadSeconds(const json::JsonObject& object, const wchar_t* key) {
    const double number = object.GetNamedNumber(key, 0);
    if (!(number >= 0 && number <= 9007199254740991.0) ||
        number != static_cast<std::int64_t>(number))
        return 0;
    return static_cast<std::int64_t>(number);
}

nx::Identity ParseIdentity(const json::JsonObject& object) {
    nx::Identity value;
    const auto data = object.GetNamedObject(L"account", object);
    // PID is an integer, not a floating-point account ID. Reject imprecise JSON numbers.
    value.pid = static_cast<std::uint64_t>(ReadSeconds(data, L"pid"));
    value.username = winrt::to_string(data.GetNamedString(L"username", L""));
    value.friend_code = winrt::to_string(data.GetNamedString(L"friend_code", L""));
    value.nex_token = winrt::to_string(object.GetNamedString(L"nex_token", L""));
    value.access_token = winrt::to_string(object.GetNamedString(L"access_token", L""));
    value.refresh_token = winrt::to_string(object.GetNamedString(L"refresh_token", L""));
    value.access_expires_at = ReadSeconds(object, L"access_expires_at");
    value.nex_expires_at = ReadSeconds(object, L"nex_expires_at");
    value.profile_id = winrt::to_string(object.GetNamedString(L"profile_id", L""));
    return value;
}

void RemoveAccount(const nx::Identity* rejected = nullptr) {
    std::scoped_lock session_lock(session_mutex);
    std::scoped_lock lock(account_mutex);
    if (rejected && !nx::SameSession(account, *rejected))
        return;
    account = {};
    account_verified = false;
    nx::SetIdentity({});
    credentials::PasswordVault vault;
    try {
        vault.Remove(vault.Retrieve(AccountResource, L"session"));
    } catch (const winrt::hresult_error& error) {
        if (error.code() != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            throw;
    }
}

// A rejected access token only forces a refresh; the refresh endpoint decides sign-out.
void Reauthorize(const nx::Identity& rejected) {
    std::scoped_lock session_lock(session_mutex);
    std::scoped_lock lock(account_mutex);
    if (!nx::SameSession(account, rejected))
        return;
    account.access_expires_at = 0;
    account_verified = false;
}

// Serialize refresh, rotated vault writes, sign-out, and replacement logins.
bool EnsureFreshSession(bool force = false) {
    std::scoped_lock session_lock(session_mutex);
    auto identity = LinkedAccount();
    if (identity.profile_id.empty() || !nx::ValidToken(identity.refresh_token))
        return false;
    const auto now = nx::EpochSeconds();
    if (!force && account_verified && !nx::SessionNeedsRefresh(identity, now))
        return true;
    const auto tick = std::chrono::steady_clock::now();
    if (!force && tick < retry_after)
        return false;
    account_verified = false;
    nx::AuthResult result;
    if (nx::ValidOAuth(identity, now) && identity.access_expires_at - now > 300) {
        // Only the NEX token is missing or stale; keep the refresh token unrotated.
        result = !nx::FetchNexToken(identity, Exchange, now) ? nx::AuthResult::Rejected
                 : SaveAccount(identity)                    ? nx::AuthResult::Ready
                                                            : nx::AuthResult::StorageFailed;
    } else {
        result = nx::RefreshSession(identity, nx::GetConfig().client_id, Exchange, SaveAccount,
                                    [] { RemoveAccount(); }, now);
    }
    account_verified = result == nx::AuthResult::Ready;
    if (!account_verified)
        retry_after = tick + std::chrono::seconds(60);
    return account_verified;
}

void PublishPresence(const nx::Presence& presence) {
    if (!nx::IsEnabled() || !EnsureFreshSession())
        return;
    const auto identity = LinkedAccount();
    if (presence.profile_id != identity.profile_id)
        return;
    const auto config = nx::GetConfig();
    auto request = nx::SocialRequest("/api/presence", identity, config.client_id);
    if (request.url.empty())
        return;
    std::ostringstream id;
    id << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << presence.program_id;
    const std::string field(reinterpret_cast<const char*>(presence.app_field.data()),
                            presence.app_field.size());
    // The API expects ordinary base64, not base64url, for AppField.
    std::string encoded(4 * ((field.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                    reinterpret_cast<const unsigned char*>(field.data()),
                    static_cast<int>(field.size()));
    request.body = "{\"status\":" + std::to_string(presence.status) +
                   ",\"app_field\":" + nx::JsonString(encoded) +
                   ",\"app_id\":" + nx::JsonString(id.str()) + ",\"app_detail\":\"\"}";
    try {
        const auto response = Send(request);
        if (response && static_cast<unsigned>(response->StatusCode()) == 401)
            Reauthorize(identity);
    } catch (...) {
    }
}

void PollSocial(std::stop_token stop) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment {
        ~Apartment() {
            winrt::uninit_apartment();
        }
    } apartment;
    auto next_refresh = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        nx::Presence presence;
        {
            std::scoped_lock lock(nx::runtime.mutex);
            presence = nx::runtime.presence;
            nx::runtime.presence.pending = false;
        }
        if (presence.profile_id.empty())
            presence.profile_id = LinkedAccount().profile_id;
        if (presence.pending)
            PublishPresence(presence);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh) {
            try {
                RefreshFriends();
                if (!presence.pending && !presence.profile_id.empty())
                    PublishPresence(presence);
                if (nx::IsEnabled() && EnsureFreshSession()) {
                    const auto identity = LinkedAccount();
                    auto request =
                        nx::SocialRequest("/api/nex-session", identity, nx::GetConfig().client_id);
                    if (!request.url.empty()) {
                        request.body = "{}";
                        const auto response = Send(request);
                        if (response && static_cast<unsigned>(response->StatusCode()) == 401)
                            Reauthorize(identity);
                    }
                }
            } catch (...) {
            }
            next_refresh = now + std::chrono::seconds(45);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
} // namespace

std::string CurrentProfileId() {
    std::scoped_lock lock(account_mutex);
    return profile_id;
}

void Initialize(const std::filesystem::path& local_state) {
    Service::Account::ProfileManager profiles;
    {
        std::scoped_lock lock(account_mutex);
        profile_id =
            profiles.GetUser(static_cast<std::size_t>(Settings::values.current_user.GetValue()))
                .value_or(Common::UUID::MakeDefault())
                .RawString();
    }
    nx::Config config;
    const auto defaults = [&config] {
        config.client_id = std::string(nx::DefaultClientId);
        config.server_address = std::string(nx::DefaultServerAddress);
        config.nat_address = std::string(nx::DefaultNatAddress);
    };
    defaults();
    try {
        std::ifstream file(local_state / "nextendo.json");
        const std::string contents((std::istreambuf_iterator<char>(file)), {});
        if (!contents.empty() && contents.size() < 16384) {
            const auto data = json::JsonObject::Parse(winrt::to_hstring(contents));
            // Each non-empty value in nextendo.json overrides its default.
            const auto override_with = [&data](const wchar_t* key, std::string& value) {
                if (auto text = winrt::to_string(data.GetNamedString(key, L"")); !text.empty())
                    value = std::move(text);
            };
            override_with(L"server_address", config.server_address);
            override_with(L"nat_address", config.nat_address);
            override_with(L"client_id", config.client_id);
        }
        const auto values =
            winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(EnabledKey))
            config.enabled = winrt::unbox_value<bool>(values.Lookup(EnabledKey));
    } catch (...) {
        config = {};
        defaults();
    }
    nx::Configure(std::move(config));
    // Provisioned Nextendo key only; no lookup of console keys, firmware, or PRODINFO.
    try {
        auto credential = credentials::PasswordVault().Retrieve(KeyResource, L"baas");
        credential.RetrievePassword();
        signer = std::make_shared<nx::IdTokenSigner>(winrt::to_string(credential.Password()));
    } catch (...) {
        try {
            signer = std::make_shared<nx::IdTokenSigner>();
        } catch (...) {
            signer.reset();
        }
    }
    // The library reopens over a running game; keep the live session after the first restore.
    if (social_worker.joinable())
        return;
    nx::Identity restored;
    try {
        auto credential = credentials::PasswordVault().Retrieve(AccountResource, L"session");
        credential.RetrievePassword();
        restored = ParseIdentity(json::JsonObject::Parse(credential.Password()));
        if (restored.profile_id.empty() || !nx::ValidToken(restored.refresh_token))
            restored = {};
    } catch (...) {
    }
    {
        std::scoped_lock lock(account_mutex);
        account = restored;
    }
    account_verified = false;
    // Linked at once; the social worker fetches the tokens on its first pass, off the UI thread.
    nx::SetIdentity(std::move(restored), Provider());
    social_worker = std::jthread(PollSocial);
}

bool SetEnabled(bool enabled) {
    try {
        winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values().Insert(
            EnabledKey, winrt::box_value(enabled));
        auto config = nx::GetConfig();
        config.enabled = enabled;
        nx::Configure(std::move(config));
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveAccount(const nx::Identity& identity) {
    if (identity.profile_id.empty() || !nx::ValidOAuth(identity, nx::EpochSeconds()) ||
        (!identity.nex_token.empty() && !nx::ValidToken(identity.nex_token)))
        return false;
    try {
        std::scoped_lock session_lock(session_mutex);
        std::scoped_lock lock(account_mutex);
        json::JsonObject data, root;
        data.SetNamedValue(L"pid",
                           json::JsonValue::CreateNumberValue(static_cast<double>(identity.pid)));
        data.SetNamedValue(
            L"username", json::JsonValue::CreateStringValue(winrt::to_hstring(identity.username)));
        data.SetNamedValue(L"friend_code", json::JsonValue::CreateStringValue(
                                               winrt::to_hstring(identity.friend_code)));
        root.SetNamedValue(L"account", data);
        root.SetNamedValue(L"profile_id", json::JsonValue::CreateStringValue(
                                              winrt::to_hstring(identity.profile_id)));
        // Only the refresh token is persisted: the vault caps a credential near 2.5 KB, and
        // the access and NEX tokens are fetched again at start.
        root.SetNamedValue(L"refresh_token", json::JsonValue::CreateStringValue(
                                                 winrt::to_hstring(identity.refresh_token)));
        // Memory first: a rotated refresh token must survive a failed vault write.
        account = identity;
        account_verified = identity.pid != 0 && nx::ValidToken(identity.nex_token) &&
                           identity.nex_expires_at > nx::EpochSeconds();
        nx::SetIdentity(identity, Provider());
        credentials::PasswordVault vault;
        vault.Add(credentials::PasswordCredential(AccountResource, L"session", root.Stringify()));
        return true;
    } catch (...) {
        return false;
    }
}

void SignOut() {
    RemoveAccount();
}

nx::Identity LinkedAccount() {
    std::scoped_lock lock(account_mutex);
    return account.profile_id == profile_id ? account : nx::Identity{};
}

nx::HttpResponse Exchange(const nx::HttpRequest& request) {
    const bool oauth = request.url == std::string(nx::ApiOrigin) + "/api/oauth/token" &&
                       request.method == "POST" &&
                       request.content_type == "application/x-www-form-urlencoded";
    const bool nex =
        request.url == std::string(nx::ApiOrigin) + "/api/nex-token" && request.method == "GET";
    if (!oauth && !nex)
        return {};
    try {
        const auto response = Send(request);
        if (!response)
            return {};
        nx::HttpResponse result;
        result.status = static_cast<unsigned>(response->StatusCode());
        if (result.status == 200) {
            const auto object = ReadJson(*response);
            if (object) {
                result.identity = ParseIdentity(*object);
                // Documented lifetimes: one hour for OAuth, 24 hours for the NEX token.
                auto lifetime = ReadSeconds(*object, L"expires_in");
                if (lifetime == 0)
                    lifetime = oauth ? 60 * 60 : 24 * 60 * 60;
                if (lifetime > 30 * 24 * 60 * 60)
                    return {result.status, {}};
                const auto expiry = nx::EpochSeconds() + lifetime;
                if (oauth) {
                    result.identity.access_expires_at = expiry;
                    // The legacy combined response did not specify a separate NEX lifetime.
                    result.identity.nex_expires_at = nx::EpochSeconds() + 24 * 60 * 60;
                } else {
                    result.identity.nex_expires_at = expiry;
                }
            }
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool RefreshFriends() {
    if (!nx::IsEnabled() || !EnsureFreshSession())
        return false;
    const auto identity = LinkedAccount();
    const auto config = nx::GetConfig();
    const auto request = nx::SocialRequest("/api/friends", identity, config.client_id);
    if (request.url.empty())
        return false;
    try {
        const auto response = Send(request);
        if (!response)
            return false;
        if (static_cast<unsigned>(response->StatusCode()) == 401) {
            Reauthorize(identity);
            return false;
        }
        if (static_cast<unsigned>(response->StatusCode()) != 200)
            return false;
        const auto object = ReadJson(*response);
        if (!object)
            return false;
        std::vector<nx::FriendEntry> friends;
        for (const auto& item : object->GetNamedArray(L"friends")) {
            if (friends.size() >= 300)
                break;
            const auto entry = item.GetObject();
            const double pid = entry.GetNamedNumber(L"pid", 0);
            if (pid <= 0 || pid > 9007199254740991.0 || pid != static_cast<std::uint64_t>(pid))
                continue;
            const auto presence = entry.GetNamedObject(L"presence", json::JsonObject());
            const auto status = presence.GetNamedNumber(L"status", 0);
            nx::FriendEntry friend_entry;
            friend_entry.pid = static_cast<std::uint64_t>(pid);
            friend_entry.name = winrt::to_string(entry.GetNamedString(L"name", L""));
            friend_entry.status = status >= 0 && status <= 2 ? static_cast<unsigned>(status) : 0;
            const auto app_id = winrt::to_string(presence.GetNamedString(L"app_id", L""));
            if (app_id.size() == 16) {
                std::from_chars(app_id.data(), app_id.data() + app_id.size(),
                                friend_entry.program_id, 16);
            }
            const auto field = winrt::to_string(presence.GetNamedString(L"app_field", L""));
            if (!field.empty() && field.size() <= 256 && field.size() % 4 == 0 &&
                field.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") ==
                    std::string::npos) {
                std::array<unsigned char, 192> decoded{};
                const int size = EVP_DecodeBlock(
                    decoded.data(), reinterpret_cast<const unsigned char*>(field.data()),
                    static_cast<int>(field.size()));
                if (size > 0 && size <= 192)
                    std::copy_n(decoded.begin(), size, friend_entry.app_field.begin());
            }
            friends.push_back(std::move(friend_entry));
        }
        std::scoped_lock lock(nx::runtime.mutex);
        if (nx::runtime.config.enabled && nx::SameSession(nx::runtime.identity, identity))
            nx::runtime.friends = std::move(friends);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<nx::FriendEntry> Friends() {
    std::scoped_lock lock(nx::runtime.mutex);
    return nx::runtime.friends;
}

} // namespace EdenXbox::NextendoOnline
