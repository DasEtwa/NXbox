# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise std-only firmware dump discovery, archival and bounded version decoding."""

import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include "frontend_common/firmware_files.h"
using namespace FirmwareManager;
namespace fs = std::filesystem;
int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path root(argv[1]);
    const auto folders = FirmwareFolders(root / "local", {root / "usb1", root / "usb2"});
    assert(folders.size() == 6);
    assert(folders[0] == root / "local" / "firmware");
    assert(folders[1] == root / "local" / "eden" / "firmware");
    assert(folders[2] == root / "usb1" / "NXbox" / "firmware");
    assert(folders[3] == root / "usb1" / "firmware");
    assert(folders[4] == root / "usb2" / "NXbox" / "firmware");
    assert(folders[5] == root / "usb2" / "firmware");
    assert(DisplayVersion(std::string("21.2.0\0junk", 11)) == "21.2.0");
    assert(DisplayVersion(std::string("1.0.0\0", 6)) == "1.0.0");
    for (const auto text : {"", "21.2.0", ".2.0", "21..0", "21.2.", "21.2", "1.2.3.4", "bad", "21.2.0\n"}) {
        const std::string bytes = std::string(text) + '\0';
        if (std::string(text) != "21.2.0") assert(DisplayVersion(bytes).empty());
    }
    assert(DisplayVersion("21.2.0").empty());
    assert(DisplayVersion(std::string(24, '9')).empty());
    const auto source = folders[0];
    fs::create_directories(source / "nested");
    { std::ofstream(source / "b.nca") << "b"; }
    { std::ofstream(source / "a.cnmt.nca") << "a"; }
    { std::ofstream(source / "readme.txt") << "keep"; }
    { std::ofstream(source / "nested" / "c.nca") << "nested"; }
    const auto files = FirmwareFiles(source);
    assert(files.size() == 2 && files[0].filename() == "a.cnmt.nca");
    const auto archived = InstalledFolder(source);
    assert(archived == source.parent_path() / "firmware_installed");
    ArchiveFirmware(source);
    assert(!fs::exists(source) && fs::exists(archived / "readme.txt"));
    // No boot input remains, and a later install preserves the previous archived dump.
    fs::create_directories(source);
    assert(FirmwareFiles(source).empty());
    { std::ofstream(source / "new.nca") << "new"; }
    ArchiveFirmware(source);
    assert(fs::exists(source.parent_path() / "firmware_installed_1" / "new.nca"));
    assert(fs::exists(archived / "b.nca"));
    fs::create_directories(source / "fake.nca");
    bool rejected = false;
    try { FirmwareFiles(source); } catch (const fs::filesystem_error&) { rejected = true; }
    assert(rejected);
    fs::remove_all(source / "fake.nca");
    fs::create_symlink(archived / "b.nca", source / "link.nca");
    rejected = false;
    try { FirmwareFiles(source); } catch (const fs::filesystem_error&) { rejected = true; }
    assert(rejected);
    fs::remove_all(source);
    rejected = false;
    try { FirmwareFiles(source); } catch (const fs::filesystem_error&) { rejected = true; }
    assert(rejected);
}
"""


class FirmwareTests(unittest.TestCase):
    def test_usb_archive_access_is_declared(self):
        manifest = ET.parse(ROOT / "dist/nxbox/AppxManifest.xml")
        namespace = {"uap": "http://schemas.microsoft.com/appx/manifest/uap/windows10"}
        self.assertIsNotNone(
            manifest.find(".//uap:Capability[@Name='removableStorage']", namespace)
        )
        types = manifest.findall(".//uap:SupportedFileTypes/uap:FileType", namespace)
        # Every type the USB import and the USB library look for must be declared, or the
        # drive hides it from the app.
        for extension in (
            ".nca",
            ".nsp",
            ".nsz",
            ".xci",
            ".xcz",
            ".keys",
            ".zip",
            ".bin",
            ".00",
            ".99",
            ".7z",
            ".rar",
        ):
            self.assertIn(extension, [entry.text for entry in types])

    def test_uwp_uses_portable_sources_and_continues_after_failure(self):
        source = (ROOT / "src/eden_uwp/firmware.cpp").read_text()
        self.assertIn("Sources::Discover(", source)
        self.assertIn("Sources::Extract(attempt_io, source, temporary, decrypt)", source)
        failure = source.split("if (result != FirmwareManager::InstallResult::Success)", 1)[1]
        failure = failure.split('Diagnostic("FIRMWARE install version=', 1)[0]
        self.assertIn("continue;", failure)
        self.assertNotIn("break;", failure)
        self.assertIn('local_state / "firmware_source.txt"', source)
        self.assertIn("source.path.parent_path()", source)
        self.assertLess(source.index('"FIRMWARE no_nca "'), source.index("InstallFirmware("))

    def test_uwp_first_launch_takes_keys_from_the_drive_before_installing(self):
        source = (ROOT / "src/eden_uwp/firmware.cpp").read_text()
        prepare = source.split("void PrepareFirmware(", 1)[1]
        # The library imports keys after this runs; without them the first install would fail.
        self.assertLess(
            prepare.index("CopyMissingKeys(local_state, drives)"), prepare.index("ReloadKeys()")
        )
        self.assertIn('{L"prod.keys", L"title.keys"}', source)
        # Keys kept inside NXbox\ (one folder per console on a shared drive) are found first.
        self.assertIn('{L"NXbox\\\\", L"switch\\\\", L""}', source)
        self.assertIn("NameCollisionOption::FailIfExists", source)

    def test_folders_archive_and_version(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-firmware-") as directory:
            root = Path(directory)
            driver = root / "driver.cpp"
            driver.write_text(DRIVER)
            binary = root / "driver"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(driver),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary), str(root / "data")], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
