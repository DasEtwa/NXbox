# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise async-shader persistence and the boot environment loader on the host."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "eden_uwp/renderer_settings.h"
using namespace EdenXbox;
std::filesystem::path test_local;
std::vector<std::string> diagnostics;
void Diagnostic(const std::string& message) { diagnostics.push_back(message); }
int _putenv_s(const char* key, const char* value) {
    return *value ? setenv(key, value, 1) : unsetenv(key);
}
// RUNTIME_LOADER
int main(int argc, char** argv) {
    assert(argc == 2);
    test_local = argv[1];
    const auto file = test_local / "eden_settings.txt";
    assert(RendererSettings::ReadAsyncShaders(test_local));
    { std::ofstream out(file); out << "# keep\nshader_preload=0\nother=a=b\n"; }
    assert(RendererSettings::ReadAsyncShaders(test_local));
    for (const bool enabled : {false, true, false}) {
        assert(RendererSettings::SaveAsyncShaders(test_local, enabled));
        assert(RendererSettings::ReadAsyncShaders(test_local) == enabled);
        std::ifstream in(file);
        const std::string content{std::istreambuf_iterator<char>(in), {}};
        assert(content == std::string("# keep\nshader_preload=0\nother=a=b\nasync_shaders=") +
                          (enabled ? "1\n" : "0\n"));
        assert(!RendererSettings::ReadShaderPreload(test_local));
    }
    const auto boot = [](const char* value, const char* source) {
        diagnostics.clear();
        LoadRuntimeEnvironment();
        assert(std::getenv("NXBOX_D3D12_ASYNC_SHADERS"));
        assert(std::string(std::getenv("NXBOX_D3D12_ASYNC_SHADERS")) == value);
        int count = 0;
        for (const auto& message : diagnostics) {
            if (message.starts_with("ASYNC_SHADERS ")) {
                ++count;
                assert(message == std::string("ASYNC_SHADERS ") +
                                      (std::strcmp(value, "1") == 0 ? "on" : "off") +
                                      " source=" + source);
            }
        }
        assert(count == 1);
    };
    boot("0", "setting");
    assert(RendererSettings::SaveAsyncShaders(test_local, true));
    boot("1", "setting"); // A later boot must not mistake the prior setting for an override.
    const auto env_file = test_local / "nxbox_env.txt";
    { std::ofstream out(env_file); out << "# diagnostic override\nNXBOX_D3D12_ASYNC_SHADERS=0\r\n"; }
    boot("0", "env");
    assert(RendererSettings::SaveAsyncShaders(test_local, false));
    { std::ofstream out(env_file); out << "NXBOX_D3D12_ASYNC_SHADERS=1\n"; }
    boot("1", "env");
    std::filesystem::remove(env_file);
    boot("0", "setting");
    std::filesystem::remove(file);
    boot("1", "setting");
    const auto blocked = test_local / "blocked";
    { std::ofstream out(blocked); out << "file"; }
    assert(!RendererSettings::SaveAsyncShaders(blocked, false));
}
"""


class RendererSettingsAsyncTests(unittest.TestCase):
    def test_persistence_and_boot_environment(self):
        source = (ROOT / "src/eden_uwp/mesa_window.cpp").read_text()
        loader = source.split("void LoadRuntimeEnvironment() {", 1)[1].split(
            "HMODULE loader_library", 1
        )[0]
        # Substitute only the UWP LocalFolder lookup; execute the production loader body.
        loader, replacements = re.subn(
            r"const auto env_file =.*?;",
            'const auto env_file = test_local / "nxbox_env.txt";',
            loader,
            count=1,
            flags=re.DOTALL,
        )
        self.assertEqual(replacements, 1)
        driver = DRIVER.replace("// RUNTIME_LOADER", "void LoadRuntimeEnvironment() {" + loader)
        with tempfile.TemporaryDirectory(prefix="nxbox-async-settings-") as directory:
            path = Path(directory)
            cpp = path / "driver.cpp"
            binary = path / "driver"
            cpp.write_text(driver)
            subprocess.run(
                [
                    os.environ.get("CXX", "c++"),
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary), directory], check=True, timeout=10)

    def test_boot_and_ui_wiring(self):
        source = (ROOT / "src/eden_uwp/mesa_window.cpp").read_text()
        boot = source.split("MesaWindow::MesaWindow", 1)[1]
        self.assertLess(boot.index("LoadRuntimeEnvironment();"), boot.index("native_d3d12 ="))
        ui = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        self.assertIn("RendererSettings::ReadAsyncShaders(local_state_)", ui)
        self.assertIn("RendererSettings::SaveAsyncShaders(local_state_, !async_shaders_)", ui)
        self.assertLess(
            ui.index("Tr(Text::ShaderPreloadTitle)"), ui.index("Tr(Text::AsyncShadersTitle)")
        )
        self.assertLess(
            ui.index("Tr(Text::AsyncShadersTitle)"), ui.index("Tr(Text::XboxButtonIconsTitle)")
        )
