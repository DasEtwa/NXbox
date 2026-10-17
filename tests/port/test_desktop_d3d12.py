# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise CMake source selection and the SDL native D3D12 wiring without an SDK."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


class DesktopD3D12Tests(unittest.TestCase):
    def test_cmake_desktop_and_uwp_source_selection(self):
        if not shutil.which("cmake"):
            self.skipTest("cmake is required to evaluate source selection")
        text = source("src/video_core/CMakeLists.txt")
        block = text[text.index("# NXbox: Xbox/UWP-specific limits") :]
        mocks = """
macro(target_sources target visibility)
  list(APPEND selected ${ARGN})
endmacro()
macro(target_compile_definitions target visibility)
  list(APPEND definitions ${ARGN})
endmacro()
function(target_include_directories)
endfunction()
function(target_link_libraries)
endfunction()
function(add_subdirectory)
endfunction()
function(add_custom_command)
endfunction()
function(set_source_files_properties)
endfunction()
function(create_target_directory_groups)
endfunction()
"""
        cases = [
            ("Windows", "ON", True, False),
            ("Windows", "OFF", False, False),
            ("WindowsStore", "OFF", True, True),
            ("Linux", "ON", False, False),
        ]
        for system, enabled, expected, uwp in cases:
            with self.subTest(system=system, enabled=enabled), tempfile.TemporaryDirectory() as tmp:
                script = Path(tmp) / "sources.cmake"
                script.write_text(
                    mocks
                    + f'set(CMAKE_SYSTEM_NAME "{system}")\n'
                    + f"set(WIN32 {'OFF' if system == 'Linux' else 'ON'})\n"
                    + f"set(ENABLE_D3D12 {enabled})\n"
                    + block
                    + '\nfile(WRITE "${CMAKE_CURRENT_LIST_DIR}/selected.txt" "${selected}|${definitions}")\n',
                    encoding="utf-8",
                )
                result = subprocess.run(
                    ["cmake", "-P", str(script)], capture_output=True, text=True, check=False
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                selected = (Path(tmp) / "selected.txt").read_text()
                self.assertEqual("renderer_d3d12/renderer_d3d12.cpp" in selected, expected)
                self.assertEqual("NXBOX_UWP=1" in selected, uwp)
                self.assertEqual("HAS_D3D12=1" in selected, expected)
        self.assertIn("PUBLIC HAS_D3D12=1", block)

    def test_cli_selects_native_d3d12_with_sdl_hwnd(self):
        launch = source("src/core/launch_params.cpp")
        self.assertIn("{\"renderer\", required_argument, 0, 'r'}", launch)
        self.assertIn("p.renderer_backend = optarg", launch)
        self.assertIn('backend == "d3d12"', launch)
        self.assertIn("Settings::RendererBackend::D3D12", launch)
        cli = source("src/yuzu_cmd/yuzu.cpp")
        self.assertIn("case Settings::RendererBackend::D3D12:", cli)
        self.assertIn("std::make_unique<EmuWindow_SDL3_D3D12>", cli)
        window = source("src/yuzu_cmd/emu_window/emu_window_sdl3_d3d12.cpp")
        self.assertIn("SDL_PROP_WINDOW_WIN32_HWND_POINTER", window)
        self.assertIn("window_info.type = Core::Frontend::WindowSystemType::Windows", window)
        self.assertNotIn("SDL_WINDOW_OPENGL", window)
        self.assertIn("emu_window/emu_window_sdl3_d3d12.cpp", source("src/yuzu_cmd/CMakeLists.txt"))
        renderer = source("src/video_core/renderer_d3d12/renderer_d3d12.cpp")
        self.assertIn("#ifdef NXBOX_UWP", renderer)
        self.assertIn("CreateSwapChainForCoreWindow", renderer)
        self.assertIn("CreateSwapChainForHwnd", renderer)
        self.assertIn("static_cast<HWND>(info.render_surface)", renderer)

    def test_desktop_diagnostics_and_runtime_dxc(self):
        self.assertRegex(
            source("src/video_core/renderer_d3d12/d3d12_shader.cpp"),
            r"LoadLibraryExW\s*\(\s*path\.c_str\(\)",
        )
        profiler = source("src/yuzu_cmd/desktop_thread_profile.h")
        for marker in (
            "ThreadProfile::Enabled()",
            "GetThreadTimes",
            "GetThreadDescription",
            "std::chrono::seconds{5}",
            "LOG_INFO(Render",
            "THREAD_PROFILE",
            "worker.join()",
        ):
            self.assertIn(marker, profiler)
        workflow = source(".github/workflows/build-desktop-d3d12.yml")
        for marker in (
            "windows-2025",
            "prepare_dxil.py",
            "nxbox-desktop-d3d12.zip",
            "workflow_dispatch",
        ):
            self.assertIn(marker, workflow)


if __name__ == "__main__":
    unittest.main()
