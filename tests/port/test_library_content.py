# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-compiled content decisions and contracts for UWP scan/launch wiring."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <array>
#include <cassert>
#include <string>
#include "frontend_common/library_content.h"
#include "core/file_sys/nca_header_check.h"
// RESULT_STATUS
using namespace LibraryContent;
using EdenXbox::Ui::Text;
int main() {
    constexpr std::uint64_t base = 0x0100f2c0115b6000ull;
    const std::array<Meta, 1> app{{{base, Kind::Application}}};
    const std::array<Meta, 1> patch{{{base | 0x800, Kind::Patch}}};
    const std::array<Meta, 1> dlc{{{base + 0x1001, Kind::AddOnContent}}};
    const std::array<Meta, 3> bundle{{patch[0], dlc[0], app[0]}};
    const std::array<std::uint64_t, 1> bases{base};
    assert(Classify({}).kind == Kind::Unknown);
    assert(Classify(app).kind == Kind::Application);
    assert(Classify(bundle).kind == Kind::Application);
    assert(Standalone(Classify(bundle).kind) == Problem::None);
    const auto update = Classify(patch);
    const auto addon = Classify(dlc);
    assert(Standalone(update.kind) == Problem::UpdateOnly);
    assert(Standalone(addon.kind) == Problem::DlcOnly);
    assert(update.bases == std::vector<std::uint64_t>{base});
    assert(addon.bases == std::vector<std::uint64_t>{base});
    assert(Attached(update, bases) && Attached(addon, bases));
    assert(!Attached(update, {}) && !Attached(addon, {}));
    assert(!Attached(Classify(app), bases));
    const std::array<Meta, 2> unrelated{{patch[0], {base + 0x2800, Kind::Patch}}};
    assert(!Attached(Classify(unrelated), bases));
    const std::array<Meta, 2> accessories{{patch[0], dlc[0]}};
    assert(Attached(Classify(accessories), bases));
    assert(Classify(accessories).kind == Kind::Patch);
    assert(Label(Problem::UpdateOnly) == Text::UpdateWithoutBase);
    assert(Label(Problem::DlcOnly) == Text::DlcWithoutBase);
    assert(Label(Problem::Corrupted) == Text::IncompleteFile);
    assert(Label(Problem::MissingKeys) == Text::LaunchMissingKeys);
    assert(Label(Problem::MissingFirmware) == Text::FirmwareMissing);
    assert(Label(Problem::None) == Text::LaunchFailed);
    for (auto status : {ResultStatus::ErrorNoExeFS, ResultStatus::ErrorNoRomFS,
                        ResultStatus::ErrorBadNCAHeader, ResultStatus::ErrorIncorrectPFSFileSize,
                        ResultStatus::ErrorBadXCIHeader, ResultStatus::ErrorXCIMissingPartition,
                        ResultStatus::ErrorXCIMissingProgramNCA})
        assert(LoadProblem(status) == Problem::Corrupted);
    for (auto status : {ResultStatus::ErrorMissingHeaderKey, ResultStatus::ErrorMissingTitlekey,
                        ResultStatus::ErrorMissingKeyAreaKey, ResultStatus::ErrorIncorrectTitlekeyOrTitlekek})
        assert(LoadProblem(status) == Problem::MissingKeys);
    assert(LoadProblem(ResultStatus::ErrorMissingBKTRBaseRomFS) == Problem::UpdateOnly);
    assert(LoadProblem(ResultStatus::ErrorNotImplemented) == Problem::None);
    for (const auto* name : {"game.nsp", "game.NSZ", "game.xci", "game.XCZ", "game.nca"})
        assert(IsPackage(name));
    for (const auto* name : {"game.nro", "game.nso", "game.elf", "game.nsp.partial", "plain"})
        assert(!IsPackage(name));
    std::array<unsigned char, 16> zero{};
    assert(FileSys::ZeroNcaSectionHeader(zero));
    zero[15] = 1;
    assert(!FileSys::ZeroNcaSectionHeader(zero));
    assert(!FileSys::ZeroNcaSectionHeader({}));
    assert(!FileSys::ZeroNcaSectionHeader(std::span(zero).first(8)));
    assert(FileSys::NcaSectionProbeOffset(4096, 1024, 512, 8192) == 4608);
    assert(!FileSys::NcaSectionProbeOffset(4096, 1024, -1, 8192));
    assert(!FileSys::NcaSectionProbeOffset(4096, 1024, 1010, 8192));
    assert(!FileSys::NcaSectionProbeOffset(UINT64_MAX, 1024, 512, 8192));
    assert(!FileSys::NcaSectionProbeOffset(4096, UINT64_MAX, 512, 8192));
    struct Entry { bool incomplete; std::string name, path; int id; };
    std::vector<Entry> entries{{true,"A update","c",1}, {false,"Z base","z",2},
                               {true,"A update","b",3}, {false,"B base","b",4},
                               {true,"A DLC","a",5}};
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return Less(a, b, [](const auto& a, const auto& b) { return a.compare(b); });
    });
    assert(entries[0].id == 4 && entries[1].id == 2 && entries[2].id == 5);
    assert(entries[3].id == 3 && entries[4].id == 1);
}
"""


class LibraryContent(unittest.TestCase):
    def test_compiled_content_labels_order_and_header_samples(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        # Use the real loader enum without pulling the entire emulator into the host test.
        loader = (ROOT / "src/core/loader/loader.h").read_text()
        enum = re.search(r"enum class ResultStatus : u16 \{.*?\n\};", loader, re.S)[0]
        enum = enum.replace(": u16", ": unsigned short")
        with tempfile.TemporaryDirectory(prefix="library-content-") as temp:
            source = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            source.write_text(DRIVER.replace("// RESULT_STATUS", enum))
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_scan_and_failed_launch_use_the_decisions(self):
        library = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
        load = library[library.index("std::optional<GameEntry> LoadGame") :]
        self.assertLess(
            load.index("if (cache_valid)"), load.index("ReadPackage(vfs")
        )
        self.assertIn("LibraryContent::Attached(game.content, bases)", library)
        self.assertIn("/*program_headers_only=*/true", library)
        screen = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        launch = screen[screen.index("    void Launch(") :]
        self.assertLess(
            launch.index("LibraryContent::Label(game.problem)"), launch.index("result_ =")
        )
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn("status - loader_base", session)
        self.assertIn("LibraryFileProblem", session)
        self.assertIn('Diagnostic("GAME_LOAD_FAILED status="', session)
        self.assertIn("show_failure(launch_status.failure.load())", session)
        splash = (ROOT / "src/eden_uwp/ui/launch_screen.cpp").read_text()
        self.assertIn("status.failure.load()", splash)

    def test_program_inspection_stays_in_headers(self):
        nca = (ROOT / "src/core/file_sys/content_archive.cpp").read_text()
        self.assertLess(
            nca.index("if (mode == ReadMode::HeaderOnly)"), nca.index("NcaFileSystemDriver fs(")
        )
        probe = nca[nca.index("bool NCA::HasZeroSectionHeader()") :]
        probe = probe[: probe.index("bool NCA::HasValidHeader()")]
        self.assertIn("ReadBytes(16, section_offset)", probe)
        self.assertNotIn("ReadAllBytes", probe)
        nsp = (ROOT / "src/core/file_sys/submission_package.cpp").read_text()
        self.assertIn("content_meta.emplace_back(cnmt.GetTitleID(), cnmt.GetType())", nsp)
        self.assertIn("program_headers_only && rec.type == ContentRecordType::Program", nsp)

    def test_required_translations(self):
        for lang, labels in {
            "en": [
                "Update without the base game",
                "DLC without the base game",
                "File incomplete or corrupted: download it again",
            ],
            "pt_br": [
                "Atualização sem o jogo base",
                "DLC sem o jogo base",
                "Arquivo incompleto ou corrompido: baixe de novo",
            ],
        }.items():
            text = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
            for label in labels:
                self.assertIn(label, text)


if __name__ == "__main__":
    unittest.main()
