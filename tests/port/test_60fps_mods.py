# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production patch discovery, NSO patching and IPS/IPSwitch with an in-memory VFS.

No emulator build, console, game data or downloaded community patch is required.
"""

import os
import re
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


class CommunityMods(unittest.TestCase):
    def test_synthetic_nso_and_mod_paths(self):
        manager = (ROOT / "src/core/file_sys/patch_manager.cpp").read_text()
        ips = (ROOT / "src/core/file_sys/ips_layer.cpp").read_text()
        # Compile production algorithms verbatim. Replace only filesystem/controller/logging
        # dependencies, which cannot be built on the host without the emulator and WinRT.
        ips = re.sub(r"^#include .*\n", "", ips, flags=re.MULTILINE)
        declarations = (ROOT / "src/core/file_sys/ips_layer.h").read_text()
        declarations = re.sub(r"^#(?:include|pragma).*\n", "", declarations, flags=re.MULTILINE)
        helpers = "\n".join(
            function(manager, signature)
            for signature in (
                "bool ModsEnabled()",
                "std::string LowerPatchName(",
                "bool IsIPSPatch(",
                "VirtualDir FindSubdirectoryCaseless(",
            )
        )
        methods = "\n".join(
            function(manager, signature)
            for signature in (
                "void PatchManager::SetAdditionalModLoadRoots(",
                "VirtualDir PatchManager::GetModificationLoadRoot(",
                "std::vector<VirtualFile> PatchManager::CollectPatches(",
                "std::vector<Core::Memory::CheatEntry> PatchManager::CreateCheatList(",
                "std::vector<u8> PatchManager::PatchNSO(",
                "bool PatchManager::HasNSOPatch(",
            )
        )
        cheats = (ROOT / "src/core/memory/cheat_engine.cpp").read_text()
        cheat_code = "namespace Core::Memory {\n" + function(
            cheats, "std::string_view ExtractName("
        )
        cheat_code += (
            "\n" + function(cheats, "std::vector<CheatEntry> TextCheatParser::Parse(") + "\n}"
        )
        driver = (ROOT / "tests/port/mods_synthetic.cpp").read_text()
        driver = driver.replace("// PRODUCTION_CHEATS", cheat_code)
        methods = (
            function(
                manager,
                "std::optional<std::vector<Core::Memory::CheatEntry>> ReadCheatFileFromFolder(",
            )
            + "\n"
            + methods
        )
        driver = driver.replace("// PRODUCTION_DECLARATIONS", declarations)
        driver = driver.replace("// PRODUCTION_IPS", ips)
        driver = driver.replace("// PRODUCTION_HELPERS", helpers)
        driver = driver.replace("// PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="nxbox-mods-") as directory:
            source = Path(directory) / "mods.cpp"
            binary = Path(directory) / "mods"
            source.write_text(driver)
            subprocess.run(
                [
                    os.environ.get("CXX", "clang++"),
                    "-std=c++20",
                    "-DFMT_HEADER_ONLY",
                    "-I/opt/homebrew/include",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                text=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("synthetic NSO mods: PASS", result.stdout)
            print(result.stdout.strip())

    def test_frontend_connects_roots_and_cheats_are_read_once(self):
        frontend = (ROOT / "src/eden_uwp/ui/mods.cpp").read_text()
        body = function(frontend, "void ApplyDisabledMods(")
        self.assertIn("ExternalGameFolders(false)", body)
        self.assertIn('local_state / "game.txt"', body)
        self.assertIn("ModLoadPaths(local_state, game_folders)", body)
        self.assertIn("SetAdditionalModLoadRoots(std::move(roots))", body)
        manager = (ROOT / "src/core/file_sys/patch_manager.cpp").read_text()
        cheats = function(
            manager, "std::vector<Core::Memory::CheatEntry> PatchManager::CreateCheatList("
        )
        self.assertEqual(cheats.count("ReadCheatFileFromFolder("), 1)
        reader = function(
            manager, "std::optional<std::vector<Core::Memory::CheatEntry>> ReadCheatFileFromFolder("
        )
        self.assertIn("LowerPatchName(candidate->GetName())", reader)
        self.assertIn("NXBOX_CHEAT_LOADED", reader)
        root = function(manager, "VirtualDir PatchManager::GetModificationLoadRoot(")
        self.assertIn("if (!ModsEnabled())", root)
        self.assertIn("additional_mod_roots", root)
        self.assertIn("FindSubdirectoryCaseless(load", root)


if __name__ == "__main__":
    unittest.main()
