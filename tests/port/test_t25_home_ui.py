# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the real portable HOME gesture and NXbox setting persistence."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include "eden_uwp/home_combo.h"
#include "eden_uwp/home_ui_settings.h"
using namespace EdenXbox;
using namespace std::chrono_literals;
int main(int argc, char** argv) {
    assert(argc == 2);
    const auto t = std::chrono::steady_clock::time_point{};
    HomeComboState state;
    assert(UpdateHomeCombo(state, true, false, t) == HomeComboAction::None);
    assert(UpdateHomeCombo(state, true, true, t) == HomeComboAction::Prompt);
    assert(UpdateHomeCombo(state, true, true, t + 400ms) == HomeComboAction::None);
    // Releasing just one button cannot count as another activation.
    assert(UpdateHomeCombo(state, false, true, t + 410ms) == HomeComboAction::None);
    assert(UpdateHomeCombo(state, true, true, t + 420ms) == HomeComboAction::None);
    UpdateHomeCombo(state, false, false, t + 430ms);
    assert(UpdateHomeCombo(state, true, true, t + 500ms) == HomeComboAction::GoHome);
    for (const auto gap : {400ms, 600ms, 800ms}) {
        state = {};
        assert(UpdateHomeCombo(state, true, true, t) == HomeComboAction::Prompt);
        UpdateHomeCombo(state, false, false, t + 100ms);
        assert(UpdateHomeCombo(state, true, true, t + gap) ==
               (gap <= 600ms ? HomeComboAction::GoHome : HomeComboAction::Prompt));
    }
    // Reset/resume requires a full release, and clears the previous confirmation.
    state = {};
    state.held = true;
    assert(UpdateHomeCombo(state, true, true, t) == HomeComboAction::None);
    UpdateHomeCombo(state, false, false, t);
    assert(UpdateHomeCombo(state, true, true, t + 100ms) == HomeComboAction::Prompt);
    const std::filesystem::path local(argv[1]);
    assert(!UseOfficialSwitchInterface(local));
    { std::ofstream out(local / "eden_settings.txt");
      out << "# preserve this\nresolution_setup=5\nuse_official_switch_interface=false\n"; }
    assert(SaveOfficialSwitchInterface(local, true));
    assert(UseOfficialSwitchInterface(local));
    assert(SaveOfficialSwitchInterface(local, false));
    assert(!UseOfficialSwitchInterface(local));
    std::ifstream in(local / "eden_settings.txt");
    const std::string content{std::istreambuf_iterator<char>(in), {}};
    assert(content.find("# preserve this\nresolution_setup=5\n") != std::string::npos);
    assert(content.find("use_official_switch_interface=false\n") != std::string::npos);
    assert(content.find("use_official_switch_interface=true") == std::string::npos);
    assert(!SaveOfficialSwitchInterface(local / "missing", true));
}
"""


class HomeUiTests(unittest.TestCase):
    def test_native_combo_and_settings(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-t25-") as directory:
            folder = Path(directory)
            source = folder / "test.cpp"
            source.write_text(DRIVER)
            binary = folder / "test"
            subprocess.run(
                [
                    "c++",
                    "-std=c++20",
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
            subprocess.run([str(binary), directory], check=True)

    def test_source_contract(self):
        def read(path):
            return (ROOT / "src/eden_uwp" / path).read_text()

        self.assertIn("std::chrono::milliseconds(600)", read("home_combo.h"))
        self.assertIn('"use_official_switch_interface"', read("home_ui_settings.h"))
        self.assertIn("kOfficialSwitchInterfaceDefault = false", read("home_ui_settings.h"))
        overlay = read("ui/library_screen.cpp")
        self.assertIn("Text::BackToNXboxInterface", overlay)
        self.assertIn("SaveOfficialSwitchInterface(local_state_, false)", overlay)
        self.assertIn("renderer.Initialize(window, true)", overlay)
        self.assertIn("DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER", read("ui/renderer.cpp"))
        self.assertIn("GetEntryRaw", read("firmware.cpp"))
        self.assertIn("AppletProgramId::QLaunch", read("firmware.cpp"))
        session = " ".join(read("game_session.cpp").split())
        self.assertIn("GetFirmwareStatus().home_available", session)
        self.assertRegex(
            session,
            r"if \(official_startup\)\s+protocol_path = std::string\(SwitchHomeBootTarget\)",
        )
        self.assertIn("system.ShutdownMainProcess()", session)
        self.assertIn(
            "RunSessionOverlay(window, session_ui.return_home, session_ui.guest_home_request)",
            session,
        )
        self.assertIn("allow_official_startup = false", session)
        self.assertIn("pressed && !suppress_guest", read("gamepad.h"))
        for lang, values in {
            "en": [
                "Press again to go home",
                "Use the system interface (firmware)",
                "Requires firmware",
                "Back to NXbox interface",
            ],
            "pt_br": [
                "Aperte de novo para ir ao início",
                "Usar a interface do sistema (firmware)",
                "Requer firmware",
                "Voltar para a interface do NXbox",
            ],
        }.items():
            table = read(f"ui/lang/{lang}.inc")
            for value in values:
                self.assertIn(value, table)


if __name__ == "__main__":
    unittest.main()
