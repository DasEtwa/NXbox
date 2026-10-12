# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production save policy with synthetic trees and upload responses."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class SaveSyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxbox-t15-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "policy"
        source = Path(cls.build.name) / "policy.cpp"
        source.write_text(r"""
#include "eden_uwp/save_sync/policy.h"
#include <cassert>
#include <iostream>
using namespace EdenXbox::SaveSync::Policy;
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "names") {
        assert(SanitizeName("Zelda: Breath/World? .", 513) == "Zelda_ Breath_World_");
        assert(SanitizeName("Pokémon", 513) == "Pokémon");
        assert(SanitizeName("...", 65) == "sem-nome");
        assert(SanitizeName("\x01\"\\<>|*", 65) == "_______");
        assert(SanitizeName("abcdef", 4) == "abc");
        assert(SanitizeName("", 3) == "se");
        assert(SanitizeName("", 0).empty());
        assert(BuildCloudFolderPath("Rayman: Legends", "Player 1") == "Rayman_ Legends/Player 1");
        assert(BuildCloudFolderPath("Game", "console") == "Game/console");
        assert(BuildCloudFolderPath(std::string(600, 'a'), std::string(100, 'b')).size() < 0x220);
    } else if (mode == "paths") {
        const std::filesystem::path root = argv[2];
        const auto legacy = root / "user/save/0000000000000000/USER/0100123456789000";
        assert(EdenSavePath(root, SaveKind::Account, 0x0100123456789000, "USER", "UUID") == legacy);
        const auto future = root / "user/save/account/UUID/0100123456789000/0";
        std::filesystem::create_directories(future);
        assert(EdenSavePath(root, SaveKind::Account, 0x0100123456789001, "USER", "UUID") == future);
        assert(EdenSavePath(root, SaveKind::Device, 0x42, "USER", "UUID") == root / "user/save/0000000000000000/00000000000000000000000000000000/0000000000000042");
        assert(EdenSavePath(root, SaveKind::System, 0x42, "USER", "UUID", 0x8000000000000010, true) == root / "system/save/8000000000000010/00000000000000000000000000000000");
    } else if (mode == "conflicts") {
        assert(Decide(true, true, 0, 0, false, 0) == SyncAction::Nothing);
        assert(Decide(false, true, 1, 0, false, 0) == SyncAction::Upload);
        assert(Decide(true, false, 0, 1, false, 0) == SyncAction::Download);
        assert(Decide(false, false, 1, 1, false, 0) == SyncAction::Equal);
        assert(Decide(false, false, 2, 3, false, 0) == SyncAction::Conflict);
        assert(Decide(false, false, 2, 3, true, 1) == SyncAction::Conflict);
        assert(Decide(false, false, 2, 1, true, 1) == SyncAction::Upload);
        assert(Decide(false, false, 1, 2, true, 1) == SyncAction::Download);
        auto hash = FingerprintSeed("save.dat", 3);
        const unsigned char data[] = {0, 1, 255};
        FingerprintBytes(hash, data, 3);
        assert(hash == 0xdbc5ca25004e8f52ull);
    } else if (mode == "chunks") {
        const std::uint64_t total = UploadChunkBytes * 2 + 17;
        auto chunk = NextChunk(total, 0);
        assert(chunk.length == UploadChunkBytes);
        assert(ContentRange(chunk, total) == "bytes 0-262143/524305");
        auto offset = AcknowledgedOffset("bytes=0-262143", total);
        chunk = NextChunk(total, offset);
        assert(chunk.offset == UploadChunkBytes && chunk.length == UploadChunkBytes);
        // Lost response: a status probe can acknowledge a partial chunk.
        offset = AcknowledgedOffset("bytes=0-300000", total);
        chunk = NextChunk(total, offset);
        assert(chunk.offset == 300001 && chunk.length == total - 300001);
        chunk = NextChunk(total, UploadChunkBytes * 2);
        assert(chunk.length == 17);
        assert(ContentRange(NextChunk(total, total), total) == "bytes */524305");
        assert(AcknowledgedOffset("", total) == 0);
        for (auto range : {"bytes=1-2", "bytes=0-999999", "bytes=0-1junk", "bytes=0-18446744073709551615"}) {
            bool threw = false;
            try { AcknowledgedOffset(range, total); } catch (const std::invalid_argument&) { threw = true; }
            assert(threw);
        }
        bool threw = false;
        try { NextChunk(1, 2); } catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }
}
""")
        subprocess.run(
            ["clang++", "-std=c++20", "-I", str(ROOT / "src"), str(source), "-o", str(cls.binary)],
            check=True,
            capture_output=True,
        )

    def run_policy(self, mode):
        with tempfile.TemporaryDirectory(prefix="nxbox-t15-tree-") as tree:
            subprocess.run([str(self.binary), mode, tree], check=True, capture_output=True)

    def test_switchsave_name_sanitization(self):
        self.run_policy("names")

    def test_eden_account_device_system_paths(self):
        self.run_policy("paths")

    def test_fingerprint_and_three_way_conflicts(self):
        self.run_policy("conflicts")

    def test_resumable_upload_chunks_and_acknowledgements(self):
        self.run_policy("chunks")
