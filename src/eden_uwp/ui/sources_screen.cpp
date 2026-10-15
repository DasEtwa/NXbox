// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden headers #undef the A/W macros of the Win32 calls they
// name their methods after.
#include <windows.h>

// wingdi.h defines GetObject as a macro for GetObjectW, which would rename JsonValue::GetObject().
#ifdef GetObject
#undef GetObject
#endif

#include "eden_uwp/ui/sources_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/scope_exit.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/game_download.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/art.h"
#include "eden_uwp/ui/source_catalog.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/tinfoil_index.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;

using D2D1::RectF;
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Storage::FileIO;
using winrt::Windows::Storage::KnownFolders;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;
using winrt::Windows::Web::Http::HttpClient;

// Layout, in canvas units. The rows follow the USB import list.
constexpr float kMargin = kScreenMargin;
constexpr float kRight = kCanvasWidth - kScreenMargin;
constexpr float kTitleTop = 150.0f;
constexpr float kTitleHeight = 76.0f;
constexpr float kSubTop = 232.0f;
constexpr float kSubHeight = 30.0f;
constexpr float kListTop = 330.0f;
constexpr float kRowHeight = 104.0f;
constexpr float kRowPitch = 110.0f;
constexpr float kRowRadius = 22.0f;
constexpr float kRowPadding = 28.0f;
constexpr float kStatWidth = 220.0f;
constexpr int kVisibleRows = 5;

constexpr float kSheetLeft = 420.0f;
constexpr float kSheetWidth = 1080.0f;
constexpr float kSheetPadding = 56.0f;
constexpr float kConfirmTop = 340.0f;
constexpr float kConfirmHeight = 360.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(4200);
constexpr auto kRateWindow = std::chrono::milliseconds(500);

constexpr std::size_t kMaxEntries = Tinfoil::kMaxEntries;
constexpr auto kRequestTimeout = std::chrono::seconds(30);

struct Source {
    std::string name;
    std::string url;
};

using Kind = Tinfoil::Kind;

struct Entry {
    std::wstring name; // what the source calls it, for display
    std::wstring file; // the name it is saved under
    std::string url;   // absolute, without the #name fragment
    std::uint64_t size = 0;
    Kind kind = Kind::Other;
    std::wstring key; // case-folded destination for Windows queue collision checks
};

enum class Failure { None, Unreachable, Unsupported, SourceError };

struct FetchResult {
    std::vector<Entry> entries;
    std::wstring message; // the source's own "success" or "error" text
    Failure failure = Failure::None;
    std::optional<Tinfoil::Traversal> traversal;
    bool partial = false;
};

struct ImportResult {
    std::vector<Source> sources;
};

// State a detached worker shares with the screen. The worker owns a reference, so leaving the
// screen never has to wait for a request that is still in flight.
template <typename Result>
struct Task {
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    Result result; // written by the worker before `done` is set, read by the screen after
};

// ---- Small string helpers (UTF-8 unless noted) ----

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool IsHttpUrl(const std::string& url) {
    const std::string lower = Lower(url.substr(0, 8));
    const bool http = StartsWith(lower, "http://") && url.size() > 7;
    const bool https = StartsWith(lower, "https://") && url.size() > 8;
    if (!http && !https) {
        return false;
    }
    return std::none_of(url.begin(), url.end(),
                        [](unsigned char c) { return c <= ' ' || c == 0x7F; });
}

// The text between "://" and the next "/", "?" or "#", without credentials.
std::string HostOf(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t start = scheme + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority.erase(0, at + 1);
    }
    return authority;
}

// For the diagnostic log and the list: no credentials and no query string, which may carry a token.
std::string RedactedUrl(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t path = url.find_first_of("/?#", scheme + 3);
    std::string result = url.substr(0, scheme + 3) + HostOf(url);
    if (path != std::string::npos && url[path] == '/') {
        const std::size_t cut = url.find_first_of("?#", path);
        result += url.substr(path, cut == std::string::npos ? cut : cut - path);
    }
    return result;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 &&
            HexValue(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string Utf8(const std::wstring& text) {
    return winrt::to_string(text);
}

const wchar_t* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::Game:
        return Tr(Text::SrcTypeGame);
    case Kind::Update:
        return Tr(Text::SrcTypeUpdate);
    case Kind::Dlc:
        return Tr(Text::SrcTypeDlc);
    case Kind::Other:
        break;
    }
    return Tr(Text::SrcOther);
}

bool IsGameExtension(const std::wstring& extension) {
    return extension == L".nsp" || extension == L".nsz" || extension == L".xci" ||
           extension == L".xcz";
}

std::wstring LowerWide(std::wstring text) {
    if (text.empty())
        return text;
    std::wstring lower(text.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(),
                      static_cast<int>(text.size()), lower.data(), static_cast<int>(lower.size()),
                      nullptr, nullptr, 0) > 0)
        return lower;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::wstring ExtensionOf(const std::wstring& name) {
    const std::size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : LowerWide(name.substr(dot));
}

// The name a download is saved under: characters Windows rejects become "_", and a name without a
// game extension borrows the one of the URL so the library recognizes the file.
std::wstring DiskName(const std::wstring& display_name, const std::string& clean_url) {
    std::wstring name = display_name;
    for (wchar_t& c : name) {
        if (c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) {
            c = L'_';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {
        name.pop_back();
    }
    if (name.size() > 180) {
        name.resize(180);
    }
    if (!IsGameExtension(ExtensionOf(name))) {
        const std::size_t query = clean_url.find_first_of("?#");
        const std::wstring url_name = Widen(UrlDecode(clean_url.substr(0, query)));
        const std::wstring extension = ExtensionOf(url_name);
        if (IsGameExtension(extension)) {
            name += extension;
        }
    }
    return name.empty() ? std::wstring(L"download") : name;
}

// ---- JSON access that tolerates missing and null fields ----

std::string StringAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::String) {
            return winrt::to_string(value.GetString());
        }
    }
    return {};
}

JsonArray ArrayAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Array) {
            return value.GetArray();
        }
    }
    return JsonArray{};
}

bool ParseObject(const std::string& text, JsonObject& object) {
    try {
        return JsonObject::TryParse(winrt::to_hstring(text), object);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// ---- The saved list ----

fs::path SourcesFile(const fs::path& local_state) {
    return local_state / L"sources.json";
}

std::vector<Source> LoadSources(const fs::path& local_state) {
    std::vector<Source> sources;
    try {
        std::ifstream in(SourcesFile(local_state), std::ios::binary);
        if (!in) {
            return sources;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        JsonObject root;
        if (!ParseObject(text, root)) {
            return sources;
        }
        for (const IJsonValue& value : ArrayAt(root, L"sources")) {
            if (value.ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject item = value.as<winrt::Windows::Data::Json::JsonObject>();
            Source source;
            source.name = StringAt(item, L"name");
            source.url = StringAt(item, L"url");
            if (IsHttpUrl(source.url)) {
                if (source.name.empty()) {
                    source.name = HostOf(source.url);
                }
                sources.push_back(std::move(source));
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_LOAD failed " + winrt::to_string(error.message()));
    }
    return sources;
}

bool SaveSources(const fs::path& local_state, const std::vector<Source>& sources) {
    try {
        JsonArray list;
        for (const Source& source : sources) {
            JsonObject item;
            item.SetNamedValue(L"name", JsonValue::CreateStringValue(winrt::to_hstring(source.name)));
            item.SetNamedValue(L"url", JsonValue::CreateStringValue(winrt::to_hstring(source.url)));
            list.Append(item);
        }
        JsonObject root;
        root.SetNamedValue(L"sources", list);
        std::error_code ec;
        fs::create_directories(local_state, ec);
        const fs::path file = SourcesFile(local_state);
        const fs::path temporary = local_state / L"sources.json.tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << winrt::to_string(root.Stringify());
            out.flush();
            if (!out) {
                Diagnostic("SOURCES_SAVE write failed");
                return false;
            }
        }
        fs::rename(temporary, file, ec);
        if (ec) {
            // rename refused to replace an existing file: remove it and try once more.
            fs::remove(file, ec);
            ec.clear();
            fs::rename(temporary, file, ec);
        }
        if (ec) {
            Diagnostic("SOURCES_SAVE rename failed " + ec.message());
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_SAVE failed " + winrt::to_string(error.message()));
        return false;
    }
}

// False when the address is already in the list (compared without case or a trailing slash).
bool AddSource(std::vector<Source>& sources, Source source) {
    const auto key = [](const std::string& url) {
        std::string lower = Lower(url);
        while (!lower.empty() && lower.back() == '/') {
            lower.pop_back();
        }
        return lower;
    };
    const std::string wanted = key(source.url);
    for (const Source& existing : sources) {
        if (key(existing.url) == wanted) {
            return false;
        }
    }
    if (source.name.empty()) {
        source.name = HostOf(source.url);
    }
    sources.push_back(std::move(source));
    return true;
}

// sources.txt: one URL per line, or "name|url". Blank lines and lines starting with # are skipped.
std::vector<Source> ParseSourcesText(const std::string& text) {
    std::vector<Source> found;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF && line.size() >= 3 &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line = Trim(line.substr(3)); // a UTF-8 byte order mark
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Source source;
        const std::size_t bar = line.find('|');
        if (!IsHttpUrl(line) && bar != std::string::npos) {
            source.name = Trim(line.substr(0, bar));
            source.url = Trim(line.substr(bar + 1));
        } else {
            source.url = line;
        }
        if (IsHttpUrl(source.url)) {
            found.push_back(std::move(source));
        }
    }
    return found;
}

// Looks for sources.txt at the root of each removable drive and in its switch\ folder.
ImportResult ScanDrivesForSources(const std::atomic<bool>& cancel) {
    ImportResult result;
    try {
        const auto drives = AwaitBounded(KnownFolders::RemovableDevices().GetFoldersAsync(),
                                         std::chrono::seconds(5));
        if (!drives) {
            return result;
        }
        for (const auto& drive : *drives) {
            for (const wchar_t* relative : {L"sources.txt", L"switch\\sources.txt"}) {
                if (cancel.load()) {
                    return result;
                }
                try {
                    const auto item =
                        AwaitBounded(drive.TryGetItemAsync(relative), std::chrono::seconds(3));
                    const auto file = item && *item ? item->try_as<StorageFile>() : nullptr;
                    if (!file) {
                        continue;
                    }
                    const auto text =
                        AwaitBounded(FileIO::ReadTextAsync(file), std::chrono::seconds(10));
                    if (!text) {
                        continue;
                    }
                    for (Source& source : ParseSourcesText(winrt::to_string(*text))) {
                        result.sources.push_back(std::move(source));
                    }
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("SOURCES_IMPORT skip " + winrt::to_string(error.message()));
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_IMPORT failed " + winrt::to_string(error.message()));
    }
    Diagnostic("SOURCES_IMPORT found=" + std::to_string(result.sources.size()));
    return result;
}

// ---- Reading a source ----

// GETs a text body. False, with a log line, on a network error or an unsuccessful status.
bool HttpGetText(const HttpClient& client, const Tinfoil::Request& request, std::string& body) {
    const std::string& url = request.url;
    try {
        const winrt::Windows::Web::Http::HttpRequestMessage message{
            winrt::Windows::Web::Http::HttpMethod::Get(), Uri{winrt::to_hstring(url)}};
        for (const auto& [name, value] : request.headers) {
            void(message.Headers().TryAppendWithoutValidation(winrt::to_hstring(name),
                                                              winrt::to_hstring(value)));
        }
        const auto response = AwaitBounded(client.SendRequestAsync(message), kRequestTimeout);
        if (!response) {
            Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " timed out");
            return false;
        }
        if (!response->IsSuccessStatusCode()) {
            Diagnostic(fmt::format("SOURCES_FETCH {} status={}", RedactedUrl(url),
                                   static_cast<int>(response->StatusCode())));
            return false;
        }
        // Keep binary bytes intact so the parser can report unsupported encrypted indexes.
        const auto buffer = AwaitBounded(response->Content().ReadAsBufferAsync(), kRequestTimeout);
        if (!buffer) {
            Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " body timed out");
            return false;
        }
        body.clear();
        if (buffer->Length() != 0) {
            body.assign(reinterpret_cast<const char*>(buffer->data()), buffer->Length());
        }
        return true;
    } catch (const winrt::hresult_error&) {
        // HTTP exception messages can include request header values.
        Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " failed");
        return false;
    }
}

FetchResult FetchSource(Tinfoil::Traversal traversal, std::size_t remaining,
                        const std::atomic<bool>& cancel) {
    FetchResult result;
    HttpClient client;
    // A failure to add the header is not worth failing the request for.
    void(client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"NXbox/1.0"));
    // A page reads one nonempty index, or at most four empty/failed indexes.
    int requests = 0;
    while (!cancel.load() && result.entries.empty() && requests++ < 4) {
        const auto request = traversal.Next();
        if (!request) {
            break;
        }
        std::string body;
        if (!HttpGetText(client, *request, body)) {
            if (request->depth == 0) {
                result.failure = Failure::Unreachable;
                return result;
            }
            result.partial = true;
            continue;
        }
        const Tinfoil::Index index = Tinfoil::ParseIndex(body, request->url);
        if (index.status != Tinfoil::Status::Ok) {
            if (request->depth == 0) {
                result.failure = index.status == Tinfoil::Status::SourceError
                                     ? Failure::SourceError
                                     : Failure::Unsupported;
                result.message = Widen(index.error);
                Diagnostic("SOURCES_FETCH " + RedactedUrl(request->url) + " unusable index");
                return result;
            }
            result.partial = true;
            continue;
        }
        if (!index.success.empty()) {
            if (!result.message.empty()) {
                result.message += L"  ·  ";
            }
            result.message += Widen(index.success);
        }
        for (const Tinfoil::File& file : index.files) {
            if (result.entries.size() >= remaining) {
                break;
            }
            Entry entry;
            entry.name = Widen(file.name);
            entry.url = file.url;
            entry.file = DiskName(entry.name, entry.url);
            entry.key = LowerWide(entry.file);
            entry.size = file.size;
            entry.kind = file.kind;
            result.entries.push_back(std::move(entry));
        }
        traversal.Follow(*request, index);
    }
    if (traversal.HasNext() && result.entries.size() < remaining)
        result.traversal = std::move(traversal);
    Diagnostic("SOURCES_FETCH page files=" + std::to_string(result.entries.size()));
    return result;
}

// Runs `body(task)` on a detached thread with its own MTA apartment.
template <typename Result, typename Body>
std::shared_ptr<Task<Result>> Spawn(Body body) {
    auto task = std::make_shared<Task<Result>>();
    std::thread([task, body = std::move(body)]() mutable {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment {
                ~Apartment() {
                    winrt::uninit_apartment();
                }
            } apartment;
            body(*task);
        } catch (const winrt::hresult_error& error) {
            Diagnostic("SOURCES worker failed " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            Diagnostic(std::string("SOURCES worker failed ") + error.what());
        }
        task->done.store(true);
    }).detach();
    return task;
}

enum class Mode { List, Keyboard, Importing, Browse, Detail, Queue, Confirm };
enum class Outcome { Pending, Done, Failed, Cancelled };

struct Cursor {
    int selected = 0;
    int top = 0;
};

class SourcesScreen {
public:
    SourcesScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                  const fs::path& local_state)
        : renderer_(renderer), window_(window), input_(input), local_state_(local_state) {}

    ~SourcesScreen() {
        if (fetch_) {
            fetch_->cancel.store(true);
        }
        if (import_) {
            import_->cancel.store(true);
        }
        cancel_.store(true);
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
    }

    // Returns true when the window was closed.
    bool Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(closed_token);
        };
        Diagnostic("UI sources screen open");
        sources_ = LoadSources(local_state_);
        while (!closed_ && !leaving_) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            const Clock::time_point now = Clock::now();
            Update(now);
            renderer_.BeginFrame();
            Draw(now);
            renderer_.EndFrame();
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        Diagnostic("UI sources screen closed");
        return closed_;
    }

private:
    // ---- State ----

    int ListRows() const { return 2 + static_cast<int>(sources_.size()); }
    int BrowseRows() const {
        return static_cast<int>(visible_.size()) + (traversal_ ? 1 : 0);
    }
    bool InBrowse() const {
        return mode_ == Mode::Browse || mode_ == Mode::Detail ||
               (mode_ == Mode::Queue && queue_return_ != Mode::List) ||
               (mode_ == Mode::Keyboard && searching_);
    }
    Cursor& CurrentCursor() { return InBrowse() ? browse_cursor_ : list_cursor_; }
    int CurrentRows() const { return InBrowse() ? BrowseRows() : ListRows(); }
    bool Loading() const { return fetch_ && !fetch_->done.load(); }

    // Which source row is focused, or -1 on one of the two action rows.
    int FocusedSource() const { return list_cursor_.selected - 2; }

    void Toast(const std::wstring& text, Clock::time_point now) {
        if (text.empty()) {
            return;
        }
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    void SetMode(Mode mode) {
        mode_ = mode;
        scroll_ = Tween(static_cast<float>(CurrentCursor().top));
    }

    void OpenKeyboard(const std::wstring& initial) {
        entry_ = std::make_unique<TextEntry>(Tr(Text::SrcEnterUrl), initial);
        SetMode(Mode::Keyboard);
    }

    void StartBrowse() {
        const Source& source = sources_[static_cast<std::size_t>(FocusedSource())];
        browse_name_ = Widen(source.name);
        browse_url_ = source.url;
        tab_ = 0;
        query_.clear();
        shelf_ = Tween(0.0f);
        if (!banners_)
            banners_ = std::make_unique<BannerSource>(local_state_);
        ScanInstalled();
        LoadBrowse();
        browse_cursor_ = Cursor{};
        SetMode(Mode::Browse);
    }

    void LoadBrowse(bool more = false) {
        if (fetch_) {
            fetch_->cancel.store(true);
        }
        ++fetch_serial_;
        if (!more) {
            entries_.clear();
            packages_.clear();
            games_.clear();
            visible_.clear();
            art_.clear();
            browse_cursor_ = {};
            shelf_ = Tween(0.0f);
            fetch_message_.clear();
            partial_ = false;
            traversal_ = Tinfoil::Traversal(browse_url_);
        }
        failure_ = Failure::None;
        auto traversal = std::move(*traversal_);
        traversal_.reset();
        fetch_ = Spawn<FetchResult>(
            [traversal = std::move(traversal),
             remaining = kMaxEntries - entries_.size()](Task<FetchResult>& task) mutable {
                task.result = FetchSource(std::move(traversal), remaining, task.cancel);
            });
    }

    void RebuildCatalog() {
        std::vector<std::uint64_t> sizes;
        packages_.clear();
        for (const auto& entry : entries_) {
            packages_.push_back(Tinfoil::ParsePackage(Utf8(entry.name)));
            sizes.push_back(entry.size);
        }
        games_ = Tinfoil::Group(packages_, sizes);
        FilterCatalog();
    }

    void FilterCatalog() {
        visible_ = Tinfoil::Select(games_, packages_, static_cast<Tinfoil::Tab>(tab_),
                                   static_cast<Tinfoil::Sort>(sort_));
        const auto query = LowerWide(query_);
        visible_.erase(std::remove_if(visible_.begin(), visible_.end(),
                                      [&](auto index) {
                                          return LowerWide(Widen(games_[index].name)).find(query) ==
                                                 std::wstring::npos;
                                      }),
                       visible_.end());
        browse_cursor_.selected =
            std::clamp(browse_cursor_.selected, 0, std::max(BrowseRows() - 1, 0));
    }

    void ScanInstalled() {
        installed_.clear();
        installed_files_.clear();
        installed_sizes_.clear();
        installed_paths_.clear();
        std::error_code ec;
        for (fs::directory_iterator it(local_state_ / L"games", ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_regular_file(ec) &&
                IsGameExtension(LowerWide(it->path().extension().wstring()))) {
                const auto size = it->file_size(ec);
                if (ec || size == 0)
                    continue;
                installed_paths_.push_back(it->path());
                installed_sizes_.push_back(size);
                installed_files_.push_back(LowerWide(it->path().filename().wstring()));
                installed_.push_back(Tinfoil::ParsePackage(Utf8(it->path().filename().wstring())));
            }
        }
        // The library can identify base games whose filenames contain no title ID. Trust a
        // cache record only while its path, size and modification time still match a local file.
        ec.clear();
        for (fs::directory_iterator it(local_state_ / L"library", ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->path().extension() != L".json" || it->path().stem().wstring().size() != 16)
                continue;
            std::ifstream in(it->path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            JsonObject record;
            if (!ParseObject(text, record))
                continue;
            const fs::path path = Widen(StringAt(record, L"path"));
            const auto id = StringAt(record, L"title_id");
            for (std::size_t i = 0; i < installed_paths_.size(); ++i) {
                if (!installed_[i].title_id.empty() ||
                    LowerWide(path.lexically_normal().wstring()) !=
                        LowerWide(installed_paths_[i].lexically_normal().wstring()))
                    continue;
                if (!record.HasKey(L"size") || !record.HasKey(L"mtime"))
                    continue;
                const auto size = record.GetNamedValue(L"size");
                const auto mtime = record.GetNamedValue(L"mtime");
                if (size.ValueType() != JsonValueType::Number ||
                    mtime.ValueType() != JsonValueType::Number)
                    continue;
                const auto modified = fs::last_write_time(installed_paths_[i], ec);
                if (ec || size.GetNumber() != static_cast<double>(installed_sizes_[i]) ||
                    mtime.GetNumber() !=
                        static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                                modified.time_since_epoch())
                                                .count()))
                    continue;
                const auto package = Tinfoil::ParsePackage("[" + id + "]");
                if (package.kind == Kind::Game)
                    installed_[i] = package;
            }
        }
    }

    bool Installed(std::size_t index) const {
        const auto& entry = entries_[index];
        for (std::size_t i = 0; i < installed_.size(); ++i) {
            const bool same_file = installed_files_[i] == LowerWide(entry.file);
            const bool same_package = Tinfoil::IsInstalled(packages_[index], installed_[i]);
            // The advertised size distinguishes an old incomplete file with the same name.
            // Different formats and newer local versions can naturally have different sizes.
            const bool newer = installed_[i].version && packages_[index].version &&
                               *installed_[i].version > *packages_[index].version;
            if (same_file && entry.size > 0 && !newer && entry.size != installed_sizes_[i])
                continue;
            if (same_file || same_package)
                return true;
        }
        return false;
    }

    void AdoptFetch() {
        if (!fetch_ || !fetch_->done.load() || fetch_adopted_ == fetch_serial_)
            return;
        fetch_adopted_ = fetch_serial_;
        for (auto& entry : fetch_->result.entries)
            entries_.push_back(std::move(entry));
        if (!fetch_->result.message.empty())
            fetch_message_ = fetch_->result.message;
        failure_ = fetch_->result.failure;
        partial_ = partial_ || fetch_->result.partial;
        traversal_ = std::move(fetch_->result.traversal);
        RebuildCatalog();
    }

    void StartImport() {
        import_ = Spawn<ImportResult>([](Task<ImportResult>& task) {
            task.result = ScanDrivesForSources(task.cancel);
        });
        SetMode(Mode::Importing);
    }

    void AdoptImport(Clock::time_point now) {
        if (!import_ || !import_->done.load()) {
            return;
        }
        int added = 0;
        for (Source& source : import_->result.sources) {
            if (AddSource(sources_, std::move(source))) {
                ++added;
            }
        }
        import_.reset();
        if (added > 0) {
            SaveSources(local_state_, sources_);
            Diagnostic("SOURCES_ADD imported=" + std::to_string(added));
            Toast(std::to_wstring(added) + L" " + Tr(Text::SrcImported), now);
        } else {
            Toast(Tr(Text::SrcImportNone), now);
        }
        SetMode(Mode::List);
    }

    void StartDownload(const Entry& entry) {
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        cancel_.store(false);
        bytes_done_.store(0);
        bytes_total_.store(entry.size);
        rate_.store(0.0);
        outcome_.store(Outcome::Pending);
        const fs::path destination = local_state_ / L"games" / entry.file;
        download_thread_ = std::thread([this, url = entry.url, name = Utf8(entry.name),
                                        expected_size = entry.size, destination]() mutable {
            Outcome outcome = Outcome::Failed;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment {
                    ~Apartment() {
                        winrt::uninit_apartment();
                    }
                } apartment;
                Clock::time_point sample_at = Clock::now();
                std::uint64_t sample_bytes = 0;
                bool first = true;
                const auto progress = [&](std::uint64_t have, std::uint64_t total) {
                    bytes_done_.store(have);
                    bytes_total_.store(total);
                    const Clock::time_point at = Clock::now();
                    if (first) {
                        first = false;
                        sample_at = at;
                        sample_bytes = have;
                        return;
                    }
                    if (at - sample_at >= kRateWindow) {
                        const double seconds = std::chrono::duration<double>(at - sample_at).count();
                        rate_.store(static_cast<double>(have - sample_bytes) / seconds);
                        sample_at = at;
                        sample_bytes = have;
                    }
                };
                // Keep incomplete packages out of the library and the installed inventory.
                // DownloadFile still resumes this partial path, including on removable storage.
                fs::path target = destination;
                target += L".partial";
                std::error_code ec;
                if (expected_size > 0 && fs::exists(destination, ec) &&
                    fs::file_size(destination, ec) < expected_size && !fs::exists(target, ec)) {
                    fs::rename(destination, target, ec); // resume a pre-catalog partial download
                }
                bool ok = DownloadFile(url, target, [](const fs::path&) {}, progress, &cancel_);
                if (ok) {
                    fs::path complete = target;
                    complete.replace_extension(); // removes only the trailing .partial
                    fs::rename(target, complete, ec);
                    ok = !ec;
                }
                outcome = ok ? Outcome::Done : (cancel_.load() ? Outcome::Cancelled : Outcome::Failed);
            } catch (const winrt::hresult_error& error) {
                Diagnostic("SOURCES_DOWNLOAD worker failed " + winrt::to_string(error.message()));
            } catch (const std::exception& error) {
                Diagnostic(std::string("SOURCES_DOWNLOAD worker failed ") + error.what());
            }
            Diagnostic("SOURCES_DOWNLOAD " + name +
                       (outcome == Outcome::Done        ? " ok"
                        : outcome == Outcome::Cancelled ? " cancelled"
                                                        : " failed"));
            outcome_.store(outcome);
        });
    }

    // ---- Input ----

    void Update(Clock::time_point now) {
        AdoptFetch();
        UpdateDownload(now);
        switch (mode_) {
        case Mode::Keyboard:
            UpdateKeyboard(now);
            return;
        case Mode::Importing:
            if (input_.Pressed(Button::B)) {
                import_->cancel.store(true);
                import_.reset();
                SetMode(Mode::List);
            } else {
                AdoptImport(now);
            }
            return;
        case Mode::Detail:
            UpdateDetail(now);
            return;
        case Mode::Queue:
            UpdateQueue();
            return;
        case Mode::Confirm:
            UpdateConfirm(now);
            return;
        case Mode::Browse:
            UpdateBrowse(now);
            break;
        case Mode::List:
            UpdateList(now);
            break;
        }
        Scroll(now);
    }

    void Scroll(Clock::time_point now) {
        Cursor& cursor = CurrentCursor();
        cursor.selected = std::clamp(cursor.selected, 0, std::max(CurrentRows() - 1, 0));
        if (cursor.selected < cursor.top) {
            cursor.top = cursor.selected;
        } else if (cursor.selected > cursor.top + kVisibleRows - 1) {
            cursor.top = cursor.selected - (kVisibleRows - 1);
        }
        cursor.top = std::max(cursor.top, 0);
        if (scroll_.Target() != static_cast<float>(cursor.top)) {
            scroll_.To(static_cast<float>(cursor.top), now, kDurationPanel);
        }
    }

    void MoveCursor(int rows) {
        Cursor& cursor = CurrentCursor();
        if (input_.Pressed(Button::Up)) {
            cursor.selected = std::max(cursor.selected - 1, 0);
        }
        if (input_.Pressed(Button::Down)) {
            cursor.selected = std::min(cursor.selected + 1, std::max(rows - 1, 0));
        }
    }

    void UpdateList(Clock::time_point now) {
        if (input_.Pressed(Button::View) && !queue_.Empty()) {
            OpenQueue();
            return;
        }
        if (input_.Pressed(Button::B)) {
            leaving_ = true;
            return;
        }
        MoveCursor(ListRows());
        if (input_.Pressed(Button::X) && FocusedSource() >= 0) {
            confirm_focus_ = 1; // the safe answer
            SetMode(Mode::Confirm);
            return;
        }
        if (!input_.Pressed(Button::A)) {
            return;
        }
        if (list_cursor_.selected == 0) {
            OpenKeyboard({});
        } else if (list_cursor_.selected == 1) {
            StartImport();
        } else {
            StartBrowse();
        }
        (void)now;
    }

    void UpdateKeyboard(Clock::time_point now) {
        const TextEntry::Result result = entry_->Update(input_);
        if (result == TextEntry::Result::Editing) {
            return;
        }
        if (searching_) {
            if (result == TextEntry::Result::Confirmed) {
                query_ = entry_->Text();
                browse_cursor_ = {};
                shelf_ = Tween(0.0f);
                FilterCatalog();
            }
            searching_ = false;
            entry_.reset();
            SetMode(Mode::Browse);
            return;
        }
        const std::string typed = Trim(Utf8(entry_->Text()));
        if (result == TextEntry::Result::Cancelled || typed.empty()) {
            entry_.reset();
            SetMode(Mode::List);
            return;
        }
        if (!IsHttpUrl(typed)) {
            Toast(Tr(Text::SrcInvalidUrl), now);
            return; // the keyboard stays open with the text, to fix it
        }
        Source source;
        source.url = typed;
        if (!AddSource(sources_, std::move(source))) {
            Toast(Tr(Text::SrcDuplicate), now);
            entry_.reset();
            SetMode(Mode::List);
            return;
        }
        SaveSources(local_state_, sources_);
        Diagnostic("SOURCES_ADD " + RedactedUrl(typed));
        Toast(Tr(Text::SrcAdded), now);
        list_cursor_.selected = ListRows() - 1;
        entry_.reset();
        SetMode(Mode::List);
    }

    void UpdateConfirm(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            SetMode(Mode::List);
            return;
        }
        if (input_.Pressed(Button::Left) || input_.Pressed(Button::Right)) {
            confirm_focus_ = 1 - confirm_focus_;
        }
        if (!input_.Pressed(Button::A)) {
            return;
        }
        const int index = FocusedSource();
        if (confirm_focus_ == 0 && index >= 0 && index < static_cast<int>(sources_.size())) {
            Diagnostic("SOURCES_REMOVE " + RedactedUrl(sources_[static_cast<std::size_t>(index)].url));
            sources_.erase(sources_.begin() + index);
            SaveSources(local_state_, sources_);
            Toast(Tr(Text::SrcRemoved), now);
        }
        SetMode(Mode::List);
    }

    void OpenQueue() {
        queue_cursor_ = 0;
        queue_return_ = mode_;
        SetMode(Mode::Queue);
    }

    void UpdateBrowse(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            if (fetch_)
                fetch_->cancel.store(true);
            SetMode(Mode::List);
            return;
        }
        if (input_.Pressed(Button::View)) {
            OpenQueue();
            return;
        }
        if (input_.Pressed(Button::LeftShoulder) || input_.Pressed(Button::RightShoulder)) {
            tab_ = (tab_ + (input_.Pressed(Button::RightShoulder) ? 1 : 3)) % 4;
            browse_cursor_ = {};
            shelf_ = Tween(0.0f);
            FilterCatalog();
        }
        if (input_.Pressed(Button::Y)) {
            searching_ = true;
            entry_ = std::make_unique<TextEntry>(Tr(Text::SrcSearch), query_);
            SetMode(Mode::Keyboard);
            return;
        }
        if (input_.Pressed(Button::X)) {
            sort_ = (sort_ + 1) % 3;
            browse_cursor_ = {};
            shelf_ = Tween(0.0f);
            FilterCatalog();
        }
        if (input_.Pressed(Button::Menu) && !Loading()) {
            LoadBrowse();
            return;
        }
        // A horizontal shelf: the selected tile stays at the left margin, as in the library.
        if (input_.Pressed(Button::Left))
            browse_cursor_.selected = std::max(0, browse_cursor_.selected - 1);
        if (input_.Pressed(Button::Right))
            browse_cursor_.selected =
                std::min(std::max(0, BrowseRows() - 1), browse_cursor_.selected + 1);
        if (input_.Pressed(Button::A) && !Loading()) {
            const auto selected = static_cast<std::size_t>(browse_cursor_.selected);
            if (selected == visible_.size() && traversal_) {
                LoadBrowse(true);
            } else if (selected < visible_.size()) {
                detail_game_ = visible_[selected];
                detail_items_ = games_[detail_game_].items;
                detail_cursor_ = 0;
                for (std::size_t i = 0; i < detail_items_.size(); ++i) {
                    if (Tinfoil::Matches(packages_[detail_items_[i]].kind,
                                         static_cast<Tinfoil::Tab>(tab_))) {
                        detail_cursor_ = static_cast<int>(i);
                        break;
                    }
                }
                panel_ = Tween(0.0f);
                panel_.To(1.0f, now, kDurationPanel);
                SetMode(Mode::Detail);
            }
        }
    }

    void UpdateDetail(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            SetMode(Mode::Browse);
            return;
        }
        if (input_.Pressed(Button::View)) {
            OpenQueue();
            return;
        }
        if (input_.Pressed(Button::Up))
            detail_cursor_ = std::max(0, detail_cursor_ - 1);
        if (input_.Pressed(Button::Down))
            detail_cursor_ =
                std::min(static_cast<int>(detail_items_.size()) - 1, detail_cursor_ + 1);
        if (input_.Pressed(Button::A) && !detail_items_.empty()) {
            const auto index = detail_items_[static_cast<std::size_t>(detail_cursor_)];
            if (Installed(index)) {
                Toast(Tr(Text::SrcInstalled), now);
                return;
            }
            if (!queue_.Add(entries_[index])) {
                Toast(Tr(Text::SrcQueueFull), now);
                return;
            }
            Toast(Tr(Text::SrcQueued), now);
            if (!download_thread_.joinable())
                StartDownload(queue_.Front());
        }
    }

    void UpdateQueue() {
        if (input_.Pressed(Button::View) || (queue_.Empty() && input_.Pressed(Button::B))) {
            SetMode(queue_return_);
            return;
        }
        if (input_.Pressed(Button::Up))
            queue_cursor_ = std::max(0, queue_cursor_ - 1);
        if (input_.Pressed(Button::Down))
            queue_cursor_ =
                std::min(static_cast<int>(queue_.Items().size()) - 1, queue_cursor_ + 1);
        queue_cursor_ =
            std::clamp(queue_cursor_, 0, std::max(0, static_cast<int>(queue_.Items().size()) - 1));
        if (input_.Pressed(Button::B) && !queue_.Empty()) {
            if (queue_cursor_ == 0)
                cancel_.store(true);
            else
                queue_.Remove(static_cast<std::size_t>(queue_cursor_));
        }
    }

    void UpdateDownload(Clock::time_point now) {
        if (!download_thread_.joinable() || outcome_.load() == Outcome::Pending)
            return;
        const Outcome outcome = outcome_.load();
        download_thread_.join();
        if (outcome == Outcome::Done) {
            // DownloadFile may choose another storage location; only mark an existing local file.
            ScanInstalled();
        }
        Toast(outcome == Outcome::Done        ? Tr(Text::SrcDownloadDone)
              : outcome == Outcome::Cancelled ? Tr(Text::SrcDownloadCancelled)
                                              : Tr(Text::SrcDownloadFailed),
              now);
        queue_.Remove(0);
        if (!queue_.Empty())
            StartDownload(queue_.Front());
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        DrawHeader();
        if (InBrowse()) {
            DrawBrowse(now);
        } else {
            DrawListRows(now);
        }
        DrawTabs(renderer_, 1, false);
        DrawClock(renderer_);
        if (!queue_.Empty() && (mode_ == Mode::Browse || mode_ == Mode::List)) {
            const auto total = bytes_total_.load();
            const auto percent =
                total > 0 ? std::min<std::uint64_t>(
                                100, static_cast<std::uint64_t>(
                                         100.0 * static_cast<double>(bytes_done_.load()) /
                                         static_cast<double>(total)))
                          : 0;
            const std::wstring status = std::wstring(Tr(Text::SrcQueue)) + L" · " +
                                        std::to_wstring(queue_.Items().size()) + L" · " +
                                        std::to_wstring(percent) + L"%";
            renderer_.DrawString(status, Font::MetaMono, RectF(760.0f, 50.0f, 1470.0f, 98.0f),
                                 Theme::kTextSecondary, HAlign::Right, VAlign::Middle);
        }
        switch (mode_) {
        case Mode::Keyboard:
            entry_->Draw(renderer_, now);
            DrawToast(now);
            return; // the keyboard draws its own hints
        case Mode::Confirm:
            DrawConfirm();
            break;
        case Mode::Importing:
            DrawBusy(Tr(Text::SrcImporting));
            break;
        case Mode::Detail:
            DrawDetail(now);
            break;
        case Mode::Queue:
            DrawQueue();
            break;
        default:
            break;
        }
        DrawToast(now);
        DrawHints(renderer_, CurrentHints());
    }

    void DrawHeader() {
        const std::wstring title = InBrowse() ? browse_name_ : std::wstring(Tr(Text::SourcesRowTitle));
        renderer_.DrawString(title, Font::ModsTitle,
                             RectF(kMargin, kTitleTop, kRight, kTitleTop + kTitleHeight),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        std::wstring line = Tr(Text::SrcSubtitle);
        D2D1_COLOR_F color = Theme::kTextSecondary;
        if (InBrowse()) {
            if (Loading()) {
                line = Tr(Text::SrcLoading);
            } else if (failure_ == Failure::Unreachable) {
                line = Tr(Text::SrcUnreachable);
                color = Theme::kDangerText;
            } else if (failure_ == Failure::Unsupported) {
                line = Tr(Text::SrcUnsupported);
                color = Theme::kDangerText;
            } else if (failure_ == Failure::SourceError) {
                line = fetch_message_;
                color = Theme::kDangerText;
            } else {
                line = std::to_wstring(games_.size()) + L" " + Tr(Text::SrcGamesCount);
                if (partial_)
                    line += L"  ·  " + std::wstring(Tr(Text::SrcPartial));
                if (!fetch_message_.empty()) {
                    line += L"  ·  " + fetch_message_;
                }
            }
        }
        renderer_.DrawString(line, Font::Meta, RectF(kMargin, kSubTop, kRight, kSubTop + kSubHeight),
                             color, HAlign::Left, VAlign::Middle);
    }

    struct RowText {
        std::wstring title;
        std::wstring sub;
        std::wstring stat;
    };

    void DrawListRows(Clock::time_point now) {
        const float scroll = scroll_.Value(now);
        renderer_.PushClip(RectF(kMargin - 20.0f, kListTop - 10.0f, kRight + 20.0f,
                                 kListTop + static_cast<float>(kVisibleRows) * kRowPitch - 1.0f));
        for (int i = 0; i < ListRows(); ++i) {
            const float y = kListTop + (static_cast<float>(i) - scroll) * kRowPitch;
            if (y + kRowHeight < kListTop - 12.0f ||
                y > kListTop + static_cast<float>(kVisibleRows) * kRowPitch) {
                continue;
            }
            RowText row;
            if (i == 0) {
                row = {Tr(Text::SrcAdd), Tr(Text::SrcAddHint), L""};
            } else if (i == 1) {
                row = {Tr(Text::SrcImportUsb), Tr(Text::SrcImportHint), L""};
            } else {
                const Source& source = sources_[static_cast<std::size_t>(i - 2)];
                row = {Widen(source.name), Widen(RedactedUrl(source.url)), L""};
            }
            DrawRow(row, i, y, mode_ == Mode::List && i == list_cursor_.selected);
        }
        renderer_.PopClip();
        if (sources_.empty()) {
            renderer_.DrawString(Tr(Text::SrcEmpty), Font::Body,
                                 RectF(kMargin, kListTop + 2.0f * kRowPitch + 20.0f, kRight,
                                       kListTop + 2.0f * kRowPitch + 140.0f),
                                 Theme::kTextSecondary);
        }
    }

    struct Art {
        Image image;
        bool final = false;
        Tween fade;
    };

    Image* VisibleArt(const std::string& id, Clock::time_point now) {
        if (id.empty() || !banners_)
            return nullptr;
        auto& art = art_[id];
        banners_->Request(id);
        if (!art.final && decodes_ < 2) {
            fs::path file;
            const auto state = banners_->Get(id, file);
            if (state != BannerState::Pending) {
                art.final = true;
                if (state == BannerState::Ready) {
                    ++decodes_;
                    Pixels pixels;
                    if (renderer_.DecodeImage(ReadFileBytes(file), pixels) &&
                        renderer_.UploadImage(pixels, art.image)) {
                        art.fade = Tween(0.0f);
                        art.fade.To(1.0f, now, kDurationPanel);
                    } else
                        banners_->Discard(id);
                }
            }
        }
        return art.image.bitmap ? &art.image : nullptr;
    }

    void DrawBrowse(Clock::time_point now) {
        decodes_ = 0;
        const Text tabs[] = {Text::SrcNewGames, Text::SrcUpdates, Text::SrcDlc, Text::SrcAll};
        float left = kMargin;
        for (int i = 0; i < 4; ++i) {
            const auto label = Tr(tabs[i]);
            const float width = PillWidth(renderer_, label, false);
            DrawChoicePill(renderer_, RectF(left, 292.0f, left + width, 356.0f), label, i == tab_,
                           false);
            left += width + kPillGap;
        }
        const Text sorts[] = {Text::SrcSortName, Text::SrcSortSize, Text::SrcSortNewest};
        std::wstring description = Tr(sorts[sort_]);
        if (!query_.empty())
            description += L"  ·  " + query_;
        renderer_.DrawString(description, Font::Meta, RectF(kMargin, 375.0f, kRight, 414.0f),
                             Theme::kTextSecondary);
        renderer_.DrawString(Tr(Text::SrcBrowseHelp), Font::RowSub,
                             RectF(kMargin, 868.0f, kRight, 910.0f), Theme::kTextSecondary);
        const int selected = browse_cursor_.selected;
        const float offset = shelf_.Value(now);
        if (shelf_.Target() != static_cast<float>(selected))
            shelf_.To(static_cast<float>(selected), now, kDurationItem);
        std::vector<std::string> keep;
        renderer_.PushClip(RectF(kMargin - 24.0f, 430.0f, kRight + 20.0f, 920.0f));
        // Only the handful of tiles intersecting the viewport ask for art or own GPU images.
        const int first = std::max(0, static_cast<int>(offset) - 1);
        const int last = std::min(BrowseRows(), first + 7);
        for (int i = first; i < last; ++i) {
            const float x = kMargin + (static_cast<float>(i) - offset) * 354.0f;
            const float lift = i == selected ? 16.0f : 0.0f;
            const auto rect = RectF(x, 480.0f - lift, x + 320.0f, 660.0f - lift);
            if (rect.right < kMargin - 20.0f || rect.left > kRight + 20.0f)
                continue;
            std::wstring name;
            std::wstring meta;
            renderer_.FillRounded(rect, 22.0f, Theme::kSurface);
            if (static_cast<std::size_t>(i) < visible_.size()) {
                const auto& game = games_[visible_[static_cast<std::size_t>(i)]];
                name = Widen(game.name);
                keep.push_back(game.base_id);
                if (auto* image = VisibleArt(game.base_id, now))
                    renderer_.DrawImage(*image, rect, 22.0f, art_[game.base_id].fade.Value(now));
                else
                    renderer_.DrawString(
                        name, Font::TileLabel,
                        RectF(x + 20.0f, rect.top + 20.0f, x + 300.0f, rect.bottom - 20.0f),
                        Theme::kText, HAlign::Center, VAlign::Middle);
                meta = game.size ? FormatBytes(game.size) : Tr(Text::SrcUnknownSize);
            } else {
                name = Tr(Text::SrcLoadMore);
                renderer_.DrawString(name, Font::TileLabel, rect, Theme::kText, HAlign::Center,
                                     VAlign::Middle);
            }
            renderer_.DrawString(name, Font::Caption, RectF(x, 694.0f, x + 320.0f, 778.0f),
                                 Theme::kText);
            renderer_.DrawString(meta, Font::MetaMono, RectF(x, 798.0f, x + 320.0f, 838.0f),
                                 Theme::kTextSecondary);
            if (i == selected && mode_ == Mode::Browse)
                DrawRing(renderer_, rect, 28.0f, 1.0f);
        }
        renderer_.PopClip();
        for (auto it = art_.begin(); it != art_.end();) {
            if (std::find(keep.begin(), keep.end(), it->first) == keep.end())
                it = art_.erase(it);
            else
                ++it;
        }
        if (Loading()) {
            renderer_.DrawString(Tr(Text::SrcLoading), Font::Body,
                                 RectF(kMargin, 818.0f, kRight, 860.0f), Theme::kTextSecondary);
            if (entries_.empty())
                for (int i = 0; i < 5; ++i) {
                    const float x = kMargin + static_cast<float>(i) * 354.0f;
                    renderer_.FillRounded(RectF(x, 480.0f, x + 320.0f, 660.0f), 22.0f,
                                          Theme::kSurface);
                }
        } else if (BrowseRows() == 0 && failure_ == Failure::None) {
            renderer_.DrawString(Tr(entries_.empty() ? Text::SrcNoFiles : Text::SrcNoMatches),
                                 Font::Body, RectF(kMargin, 490.0f, kRight, 620.0f),
                                 Theme::kTextSecondary);
        }
    }

    void DrawDetail(Clock::time_point now) {
        const auto& game = games_[detail_game_];
        const float shift = (1.0f - panel_.Value(now)) * 28.0f;
        const auto sheet = RectF(300.0f, 170.0f + shift, 1620.0f, 930.0f + shift);
        DrawSheet(renderer_, sheet);
        renderer_.PushClip(sheet);
        if (auto* image = VisibleArt(game.base_id, now))
            renderer_.DrawImage(*image, RectF(348.0f, 208.0f + shift, 668.0f, 388.0f + shift),
                                22.0f, 1.0f);
        renderer_.DrawString(Widen(game.name), Font::Heading,
                             RectF(704.0f, 208.0f + shift, 1572.0f, 320.0f + shift), Theme::kText);
        renderer_.DrawString(game.size ? FormatBytes(game.size) : Tr(Text::SrcUnknownSize),
                             Font::MetaMono, RectF(704.0f, 330.0f + shift, 1572.0f, 374.0f + shift),
                             Theme::kTextSecondary);
        // Availability is independent of the active tab, including locally installed add-ons
        // that this source does not carry.
        const Kind kinds[] = {Kind::Game, Kind::Update, Kind::Dlc};
        for (int i = 0; i < 3; ++i) {
            const auto kind = kinds[i];
            const bool available =
                std::any_of(game.items.begin(), game.items.end(),
                            [&](auto item) { return packages_[item].kind == kind; });
            const bool installed =
                std::any_of(installed_.begin(), installed_.end(), [&](const auto& local) {
                    return !game.base_id.empty() && local.base_id == game.base_id &&
                           local.kind == kind;
                });
            const float x = 348.0f + static_cast<float>(i) * 408.0f;
            renderer_.DrawString(KindLabel(kind), Font::Meta,
                                 RectF(x, 404.0f + shift, x + 398.0f, 436.0f + shift),
                                 Theme::kText);
            std::wstring label = Tr(available ? Text::SrcAvailable : Text::SrcUnavailable);
            if (installed)
                label += L" · " + std::wstring(Tr(Text::SrcInstalled));
            renderer_.DrawString(label, Font::RowSub,
                                 RectF(x, 438.0f + shift, x + 398.0f, 470.0f + shift),
                                 Theme::kTextSecondary);
        }
        const int first = std::max(0, detail_cursor_ - 4);
        for (int row = first; row < std::min(first + 5, static_cast<int>(detail_items_.size()));
             ++row) {
            const auto item = detail_items_[static_cast<std::size_t>(row)];
            const auto& package = packages_[item];
            const float y = 482.0f + shift + static_cast<float>(row - first) * 76.0f;
            const auto rect = RectF(348.0f, y, 1572.0f, y + 64.0f);
            std::wstring label = KindLabel(package.kind);
            label += L" · " + (package.version ? std::wstring(Tr(Text::SrcVersion)) + L" " +
                                                     std::to_wstring(*package.version)
                                               : Tr(Text::SrcUnknownVersion));
            label += L" · " + (entries_[item].size ? FormatBytes(entries_[item].size)
                                                   : Tr(Text::SrcUnknownSize));
            if (Installed(item))
                label += L" · " + std::wstring(Tr(Text::SrcInstalled));
            renderer_.DrawString(entries_[item].name, Font::RowSub,
                                 RectF(366.0f, y + 2.0f, 1554.0f, y + 30.0f), Theme::kText);
            renderer_.DrawString(label, Font::RowSub, RectF(366.0f, y + 32.0f, 1554.0f, y + 62.0f),
                                 Theme::kTextSecondary);
            if (row == detail_cursor_)
                DrawRing(renderer_, rect, 22.0f, 1.0f);
        }
        renderer_.PopClip();
    }

    void DrawQueue() {
        DrawSheet(renderer_, RectF(300.0f, 170.0f, 1620.0f, 930.0f));
        renderer_.DrawString(Tr(Text::SrcQueue), Font::Heading,
                             RectF(348.0f, 202.0f, 1572.0f, 270.0f), Theme::kText);
        DrawProgress();
        if (queue_.Empty())
            renderer_.DrawString(Tr(Text::SrcQueueEmpty), Font::Body,
                                 RectF(348.0f, 400.0f, 1572.0f, 550.0f), Theme::kTextSecondary);
        for (std::size_t i = 0; i < queue_.Items().size(); ++i) {
            const float y = 358.0f + static_cast<float>(i) * 66.0f;
            renderer_.DrawString(queue_.Items()[i].name, Font::RowSub,
                                 RectF(366.0f, y, 1300.0f, y + 48.0f), Theme::kText, HAlign::Left,
                                 VAlign::Middle);
            renderer_.DrawString(Tr(i == 0 ? Text::SrcDownloading : Text::SrcWaiting), Font::RowSub,
                                 RectF(1320.0f, y, 1554.0f, y + 48.0f), Theme::kTextSecondary,
                                 HAlign::Right, VAlign::Middle);
            if (static_cast<int>(i) == queue_cursor_)
                DrawRing(renderer_, RectF(348.0f, y, 1572.0f, y + 48.0f), 22.0f, 1.0f);
        }
    }

    void DrawRow(const RowText& row, int index, float y, bool focused) {
        const D2D1_RECT_F rect = RectF(kMargin, y, kRight, y + kRowHeight);
        const Cursor& cursor = InBrowse() ? browse_cursor_ : list_cursor_;
        if (focused) {
            renderer_.FillRounded(rect, kRowRadius, Theme::kSurfaceStrong);
        } else if (index > 0 && index - 1 != cursor.selected) {
            renderer_.FillRounded(RectF(kMargin, y - 3.0f, kRight, y - 2.0f), 0.0f,
                                  Theme::kHairline);
        }
        const float left = kMargin + kRowPadding;
        const float stat_right = kRight - kRowPadding;
        const float info_right = row.stat.empty() ? stat_right : stat_right - kStatWidth - 24.0f;
        renderer_.DrawString(row.title, Font::RowTitle,
                             RectF(left, y + 20.0f, info_right, y + 54.0f), Theme::kText,
                             HAlign::Left, VAlign::Middle);
        renderer_.DrawString(row.sub, Font::RowSub, RectF(left, y + 58.0f, info_right, y + 84.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        if (!row.stat.empty()) {
            renderer_.DrawString(row.stat, Font::Stat,
                                 RectF(stat_right - kStatWidth, y, stat_right, y + kRowHeight),
                                 Theme::kText, HAlign::Right, VAlign::Middle);
        }
        if (focused) {
            DrawRing(renderer_, rect, kRowRadius + kRingGap, 1.0f);
        }
    }

    void DrawConfirm() {
        const int index = FocusedSource();
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kConfirmTop, kSheetLeft + kSheetWidth, kConfirmTop + kConfirmHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(Tr(Text::SrcRemoveTitle), Font::Heading,
                             RectF(left, sheet.top + 36.0f, right, sheet.top + 100.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        if (index >= 0 && index < static_cast<int>(sources_.size())) {
            renderer_.DrawString(Widen(sources_[static_cast<std::size_t>(index)].name), Font::Meta,
                                 RectF(left, sheet.top + 108.0f, right, sheet.top + 142.0f),
                                 Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        }
        renderer_.DrawString(Tr(Text::SrcRemoveBody), Font::Body,
                             RectF(left, sheet.top + 156.0f, right, sheet.top + 250.0f),
                             Theme::kTextSecondary);
        const std::wstring remove = Tr(Text::SrcRemove);
        const std::wstring keep = Tr(Text::HintCancel);
        const float remove_width = PillWidth(renderer_, remove, false);
        const float keep_width = PillWidth(renderer_, keep, false);
        const float top = sheet.top + 262.0f;
        DrawChoicePill(renderer_, RectF(left, top, left + remove_width, top + kPillHeight), remove,
                       false, confirm_focus_ == 0);
        const float keep_left = left + remove_width + kPillGap;
        DrawChoicePill(renderer_, RectF(keep_left, top, keep_left + keep_width, top + kPillHeight),
                       keep, true, confirm_focus_ == 1);
    }

    void DrawBusy(const std::wstring& text) {
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kConfirmTop, kSheetLeft + kSheetWidth, kConfirmTop + 220.0f);
        DrawSheet(renderer_, sheet);
        renderer_.DrawString(text, Font::Heading,
                             RectF(sheet.left + kSheetPadding, sheet.top, sheet.right - kSheetPadding,
                                   sheet.bottom),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    // The active transfer uses the existing progress bar, percentage, rate and byte counters.
    void DrawProgress() {
        const std::uint64_t done = bytes_done_.load();
        const std::uint64_t total = bytes_total_.load();
        const double fraction = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
        if (queue_.Empty())
            return;
        const float left = 348.0f;
        const float right = 1572.0f;
        DrawProgressBar(renderer_, RectF(left, 286.0f, right, 298.0f),
                        std::clamp(fraction, 0.0, 1.0));
        const double rate = rate_.load();
        wchar_t speed[32];
        swprintf_s(speed, std::size(speed), L"%.1f", rate / (1024.0 * 1024.0));
        std::wstring stats = std::to_wstring(static_cast<int>(fraction * 100.0)) + L"%   " + speed +
                             L" " + Tr(Text::SrcRate);
        if (CurrentLanguage() == Language::Portuguese) {
            std::replace(stats.begin(), stats.end(), L'.', L',');
        }
        stats += L"   " + FormatBytes(done) + L" / " + FormatBytes(total);
        renderer_.DrawString(stats, Font::MetaMono, RectF(left, 308.0f, right, 344.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        DrawToastCapsule(renderer_, toast_,
                         std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f));
    }

    std::vector<Hint> CurrentHints() const {
        switch (mode_) {
        case Mode::Queue:
            return {{Theme::kButtonB, L"B", Tr(Text::SrcCancelEntry)},
                    {Theme::kTextSecondary, L"≡", Tr(Text::HintBack)}};
        case Mode::Detail:
            return {{Theme::kButtonA, L"A", Tr(Text::HintDownload)},
                    {Theme::kTextSecondary, L"≡", Tr(Text::SrcQueue)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Importing:
            return {{Theme::kButtonB, L"B", Tr(Text::HintCancel)}};
        case Mode::Confirm:
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Browse:
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonY, L"Y", Tr(Text::SrcSearch)},
                    {Theme::kTextSecondary, L"≡", Tr(Text::SrcQueue)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Keyboard:
            break;
        case Mode::List:
            break;
        }
        std::vector<Hint> hints = {{Theme::kButtonA, L"A", Tr(Text::HintSelect)}};
        if (!queue_.Empty())
            hints.push_back({Theme::kTextSecondary, L"≡", Tr(Text::SrcQueue)});
        if (FocusedSource() >= 0) {
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::HintRemove)});
        }
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
        return hints;
    }

    // ---- Members ----

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    fs::path local_state_;

    Mode mode_ = Mode::List;
    std::vector<Source> sources_;
    Cursor list_cursor_;
    Cursor browse_cursor_;
    Tween scroll_;
    int confirm_focus_ = 1; // 0 remove, 1 keep
    std::unique_ptr<TextEntry> entry_;

    std::wstring browse_name_;
    std::string browse_url_;
    std::vector<Entry> entries_;
    std::vector<Tinfoil::Package> packages_;
    std::vector<Tinfoil::Game> games_;
    std::vector<std::size_t> visible_;
    std::vector<Tinfoil::Package> installed_;
    std::vector<std::wstring> installed_files_;
    std::vector<std::uint64_t> installed_sizes_;
    std::vector<fs::path> installed_paths_;
    std::optional<Tinfoil::Traversal> traversal_;
    std::unique_ptr<BannerSource> banners_;
    std::map<std::string, Art> art_;
    int decodes_ = 0;
    int tab_ = 0;
    int sort_ = 0;
    bool partial_ = false;
    std::wstring query_;
    bool searching_ = false;
    Tween shelf_;
    Tween panel_;
    std::size_t detail_game_ = 0;
    std::vector<std::size_t> detail_items_;
    int detail_cursor_ = 0;
    Tinfoil::DownloadQueue<Entry> queue_;
    int queue_cursor_ = 0;
    Mode queue_return_ = Mode::Browse;
    std::wstring fetch_message_;
    Failure failure_ = Failure::None;
    std::shared_ptr<Task<FetchResult>> fetch_;
    int fetch_serial_ = 0;   // which fetch the screen started last
    int fetch_adopted_ = 0;  // which fetch's result it already took
    std::shared_ptr<Task<ImportResult>> import_;

    std::wstring toast_;
    Clock::time_point toast_until_{};
    bool closed_ = false;
    bool leaving_ = false;

    // The download worker. Joined by the screen, never detached: it writes into the file system.
    std::thread download_thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<std::uint64_t> bytes_done_{0};
    std::atomic<std::uint64_t> bytes_total_{0};
    std::atomic<double> rate_{0.0};
    std::atomic<Outcome> outcome_{Outcome::Pending};
};

} // namespace

bool RunSourcesScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                      const std::filesystem::path& local_state) {
    try {
        SourcesScreen screen(renderer, window, input, local_state);
        return screen.Run();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI sources screen failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI sources screen failed ") + error.what());
    }
    return false;
}

int CountSources(const std::filesystem::path& local_state) {
    return static_cast<int>(LoadSources(local_state).size());
}

} // namespace EdenXbox::Ui
