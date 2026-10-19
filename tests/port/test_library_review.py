# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production scan with counted package reads and a host filesystem."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r"""
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include "frontend_common/library_content.h"
namespace fs = std::filesystem;
using EdenXbox::Ui::Text;
namespace EdenXbox::Ui { const wchar_t* Tr(Text) { return L"label"; } }
std::wstring Lowercase(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s;
}
std::string Utf8(const std::wstring& s) { return {s.begin(), s.end()}; }
std::string IconLanguage() { return "AmericanEnglish"; }
void Diagnostic(const std::string&) {}
namespace fmt {
template<class T> std::string format(const char*, T) { return "0000000000000001"; }
}
namespace Common::FS {
std::uint64_t GetSize(const fs::path& p) { return fs::file_size(p); }
}
std::int64_t modified = 1;
std::int64_t ModifiedSeconds(const fs::path&) { return modified; }
namespace FileSys { struct RealVfsFilesystem {}; }
namespace Art {
bool WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}
}
std::size_t bytes_read = 0;
void ReadBytes(const fs::path& path, std::size_t count) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<char> data(count);
    stream.read(data.data(), static_cast<std::streamsize>(count));
    bytes_read += static_cast<std::size_t>(stream.gcount());
}
// TYPES
CacheRecord saved;
bool WriteRecord(const fs::path&, const GameEntry& game) {
    saved.title_id = game.title_id; saved.name = game.name; saved.path = game.path.wstring();
    saved.size = game.size; saved.mtime = game.mtime; saved.nacp_names = game.nacp_names;
    saved.icon_language = IconLanguage();
    // CLASSIFICATION
    return true;
}
LibraryContent::Problem next_problem = LibraryContent::Problem::None;
std::string ReadPackage(FileSys::RealVfsFilesystem&, const fs::path& p, PackageInfo& info, bool) {
    ReadBytes(p, 4096);
    info.title_id = 1; info.name = L"Game"; info.problem = next_problem; info.icon = {1};
    info.content.kind = LibraryContent::Kind::Application;
    return {};
}
std::optional<bool> TruncatedPackage(FileSys::RealVfsFilesystem&, const fs::path& p) {
    ReadBytes(p, 16); return false;
}
// FUNCTIONS
int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path dir(argv[1]);
    const auto path = dir / "game.nsp";
    { std::ofstream out(path); out << std::string(8192, 'x'); }
    FileSys::RealVfsFilesystem vfs;
    ScanStats stats;
    auto game = LoadGame(vfs, dir, {}, path, stats);
    assert(game && !game->incomplete);
    std::map<std::wstring, CacheRecord> cache{{path.wstring(), saved}};
    bytes_read = 0;
    game = LoadGame(vfs, dir, cache, path, stats);
    std::cout << "warm scan package bytes=" << bytes_read << std::endl;
    assert(bytes_read == 0);
    assert(game && game->content.kind == LibraryContent::Kind::Application);
    modified = 2;
    bytes_read = 0;
    next_problem = LibraryContent::Problem::MissingKeys;
    game = LoadGame(vfs, dir, cache, path, stats);
    assert(bytes_read > 0 && game && game->problem == next_problem);
    cache[path.wstring()] = saved;
    bytes_read = 0;
    game = LoadGame(vfs, dir, cache, path, stats);
    // Missing keys are re-checked every scan: the player can add keys without touching the file.
    assert(bytes_read > 0 && game && game->incomplete && game->problem == next_problem);
    { std::ofstream out(path, std::ios::app); out << 'x'; }
    bytes_read = 0;
    game = LoadGame(vfs, dir, cache, path, stats);
    assert(bytes_read > 0);
    bytes_read = 0;
    assert(LibraryFileProblem(dir / "homebrew.nro") == LibraryContent::Problem::None);
    assert(bytes_read == 0);
}
"""


class LibraryReview(unittest.TestCase):
    def scan_driver(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        header = (ROOT / "src/eden_uwp/ui/library.h").read_text()
        types = "\n".join(
            function(source, "struct " + name) + ";"
            for source, name in [
                (library, "CacheRecord"),
                (library, "PackageInfo"),
                (library, "ScanStats"),
                (header, "GameEntry"),
            ]
        )
        driver = PRELUDE.replace("// TYPES", types).replace(
            "// CLASSIFICATION",
            "saved.content = game.content; saved.problem = game.problem; "
            "saved.has_icon = !game.icon.empty();"
            if "bool has_icon" in types
            else "",
        )
        driver = driver.replace(
            "// FUNCTIONS",
            function(library, "std::optional<GameEntry> LoadGame")
            + "\n"
            + function(library, "LibraryContent::Problem LibraryFileProblem"),
        )
        return driver

    def test_scan_reads_zero_package_bytes_when_warm(self):
        self.compile_run(self.scan_driver())

    def test_non_package_launch_does_not_open_package_data(self):
        driver = (
            self.scan_driver().split("int main(int argc")[0]
            + r"""
int main() {
    next_problem = LibraryContent::Problem::Corrupted;
    for (const auto* name : {"homebrew.nro", "homebrew.NSO", "homebrew.elf", "game.nsp.partial"}) {
        assert(LibraryFileProblem(name) == LibraryContent::Problem::None);
        assert(bytes_read == 0);
    }
}
"""
        )
        self.compile_run(driver)

    def test_failed_launch_uses_utf8_path(self):
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        expression = re.search(r"Ui::LibraryFileProblem\((.+)\);", session)[1]
        # Both constructors behave identically on POSIX; MSVC needs the explicit UTF-8 conversion.
        self.assertEqual(expression, "std::filesystem::u8path(path)")
        driver = r"""
#include <cassert>
#include <filesystem>
#include <string>
int main() {
    const std::string path = "D:/Jogos/João/日本語/game.nsp";
    const auto converted = EXPRESSION;
    const auto encoded = converted.u8string();
    assert(std::string(encoded.begin(), encoded.end()) == path);
#ifdef _WIN32
    assert(converted.wstring() == L"D:/Jogos/João/日本語/game.nsp");
#endif
}
"""
        self.compile_run(driver.replace("EXPRESSION", expression), standard="c++17")

    def test_unknown_metadata_is_skipped_before_program_checks(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        # Execute the actual metadata decision block with an unknown system CNMT.
        start = library.index("    info.content = LibraryContent::Classify(meta);")
        end = library.index("    if (!read_details)", start)
        block = library[start:end]
        types = function(library, "struct PackageInfo") + ";"
        driver = (
            PRELUDE[: PRELUDE.index("// TYPES")]
            + types
            + r"""
namespace Loader { enum class ResultStatus { Success }; }
namespace Core::Crypto {
enum class S256KeyType { Header };
struct KeyManager {
    static KeyManager& Instance() { static KeyManager keys; return keys; }
    bool HasKey(S256KeyType) { return false; }
};
}
bool IsKeyError(Loader::ResultStatus) { return false; }
struct Nsp {
    bool HasCorruptProgram() { return true; }
    bool HasMissingProgramKeys() { return true; }
    auto GetNCAs() { return std::map<int, std::map<std::pair<int, int>, int>>{}; }
    Loader::ResultStatus GetProgramStatus() { return Loader::ResultStatus::Success; }
};
namespace FileSys {
enum TitleType { Application };
enum ContentRecordType { Program };
}
std::string Check(PackageInfo& info) {
    std::vector<LibraryContent::Meta> meta{{42, LibraryContent::Kind::Unknown}};
    Nsp package; auto* nsp = &package;
// BLOCK
    return {};
}
int main() {
    PackageInfo info;
    assert(!Check(info).empty());
    assert(info.problem == LibraryContent::Problem::None && !info.keys_problem);
}
"""
        )
        # The production fallback formats loader errors after this decision.
        driver = driver.replace(
            "namespace Loader { enum class ResultStatus { Success }; }",
            "namespace Loader { enum class ResultStatus { Success }; "
            'std::string GetResultStatusString(ResultStatus) { return "status"; } }',
        )
        self.compile_run(driver.replace("// BLOCK", block))

    def test_failed_screen_hint_and_back_input(self):
        screen = (ROOT / "src/eden_uwp/ui/launch_screen.cpp").read_text()
        header = (ROOT / "src/eden_uwp/ui/launch_screen.h").read_text()
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        draw = function(screen, "void LaunchScreen::Draw")
        back = function(session, "    const auto show_failure =") + ";"
        driver = r"""
#include <atomic>
#include <cassert>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "eden_uwp/ui/strings.h"
using namespace EdenXbox::Ui;
// STATUS
struct Color { float a = 1; };
namespace Theme { Color kBackground, kText, kTextSecondary, kSurfaceStrong; }
struct D2D1_GRADIENT_STOP { float position; Color color; };
namespace D2D1 {
struct Rect {};
Rect RectF(float, float, float, float) { return {}; }
int Point2F(float, float) { return 0; }
}
enum class Font { Body, TitleSmall };
struct Pixels {};
namespace EdenXbox::Ui {
const wchar_t* Tr(Text text) {
    return text == Text::LaunchBackHint ? L"back" : L"status";
}
}
std::vector<std::wstring> labels;
struct Renderer {
    void BeginFrame() {}
    void EndFrame(Pixels*) {}
    template<class T> void DrawImage(const T&, D2D1::Rect, int, int) {}
    void FillGradient(D2D1::Rect, int, const D2D1_GRADIENT_STOP*, int, int, int) {}
    void FillRounded(D2D1::Rect, float, Color) {}
    void DrawString(const std::wstring& text, Font, D2D1::Rect, Color) { labels.push_back(text); }
};
struct LaunchScreen {
    std::unique_ptr<Renderer> renderer_ = std::make_unique<Renderer>();
    struct { bool bitmap = false; } art_;
    std::wstring name_ = L"game";
    template<class T> LaunchScreen(const T&, int, int) {}
    void LoadArt() {}
    void Draw(const LaunchStatus&, Text = Text::LaunchFailed, Pixels* = nullptr);
};
// DRAW
namespace Ui {
using ::LaunchScreen; using ::LaunchPhase; using ::Text;
enum class Button { B };
int polls = 0;
struct Input {
    template<class T> explicit Input(const T&) {}
    void Update() { ++polls; }
    bool Pressed(Button button) { assert(button == Button::B); return polls == 2; }
};
}
enum class CoreProcessEventsOption { ProcessAllIfPresent };
struct Dispatcher {
    void ProcessEvents(CoreProcessEventsOption) { assert(Ui::polls < 3); }
};
struct Window { ::Dispatcher Dispatcher() { return {}; } };
int main() {
    LaunchStatus status;
    LaunchScreen screen(0, 0, 0);
    status.phase.store(LaunchPhase::Failed);
    screen.Draw(status);
    assert(std::find(labels.begin(), labels.end(), L"back") != labels.end());
    labels.clear();
    status.phase.store(LaunchPhase::Lookup);
    screen.Draw(status);
    assert(std::find(labels.begin(), labels.end(), L"back") == labels.end());
    std::atomic<bool> closed = false;
    std::unique_ptr<LaunchScreen> splash;
    Window window;
    int local_state = 0, protocol_title = 0;
    LaunchStatus launch_status;
// BACK
    show_failure(Text::LaunchFailed);
    assert(Ui::polls == 2 && !closed);
}
"""
        driver = (
            driver.replace(
                "// STATUS",
                function(header, "enum class LaunchPhase")
                + ";\n"
                + function(header, "struct LaunchStatus")
                + ";",
            )
            .replace("// DRAW", draw)
            .replace("// BACK", back)
        )
        self.compile_run(driver)

    def compile_run(self, driver, standard="c++20"):
        compiler = shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="library-review-") as temp:
            source, binary = Path(temp) / "test.cpp", Path(temp) / "test"
            source.write_text(driver)
            subprocess.run(
                [
                    compiler,
                    f"-std={standard}",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary), temp], check=True)


if __name__ == "__main__":
    unittest.main()
