# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the shop's real metadata, grouping, inventory and queue logic on the host."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <string>
#include <vector>
#include "eden_uwp/ui/source_catalog.h"
using namespace EdenXbox::Tinfoil;
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string test = argv[1];
    if (test == "metadata") {
        const auto base = ParsePackage("Pokémon [0100aabbcc002000][v0].nsp");
        assert(base.name == "Pokémon" && base.title_id == "0100AABBCC002000");
        assert(base.base_id == base.title_id && base.kind == Kind::Game && base.version == 0);
        const auto update = ParsePackage("Pokémon [0100AABBCC002800][v131072].nsz");
        assert(update.base_id == base.title_id && update.kind == Kind::Update);
        assert(update.version == 131072);
        const auto dlc = ParsePackage("Expansion [0100AABBCC003123][v65536].nsp");
        assert(dlc.base_id == base.title_id && dlc.kind == Kind::Dlc);
        assert(ParsePackage("Legacy [0100AABBCC002000][v1].nsp").kind == Kind::Update);
        assert(ParsePackage("Game [0100AABBCC002800][v0].nsp").kind == Kind::Update);
        assert(ParsePackage("Odd [0100AABBCC002001].nsp").base_id == base.title_id);
    } else if (test == "malformed_and_fallback") {
        for (const auto file : {"x [not an id].nsp", "x [0100AABBCC00200Z].nsp", "x [v-1]", "x [v99999999999999999999999]", "x [v]", "x [v1"}) {
            const auto package = ParsePackage(file);
            assert(package.title_id.empty() && !package.version);
        }
        assert(ParsePackage("Plain game.xci").name == "Plain game");
        assert(ParsePackage("[0100AABBCC002000].nsp").name == "[0100AABBCC002000].nsp");
        assert(ParsePackage("DLC [v0].nsp").kind == Kind::Dlc);
        assert(ParsePackage("Game [v999999999].nsp").version == 999999999);
    } else if (test == "group_and_tabs") {
        const std::vector<Package> packages = {
            ParsePackage("Patch [0100AABBCC002800][v1].nsp"),
            ParsePackage("Expansion [0100AABBCC003001][v0].nsp"),
            ParsePackage("Game [0100AABBCC002000][v0].nsp"),
            ParsePackage("Patch only [0100AABBCC004800][v1].nsp"),
            ParsePackage("Same.nsp"), ParsePackage("Same.nsp"),
        };
        const auto games = Group(packages, {2, 3, 5, 7, 11, 13});
        assert(games.size() == 4 && games[0].items == std::vector<std::size_t>({0, 1, 2}));
        assert(games[0].name == "Game" && games[0].size == 10);
        assert(Select(games, packages, Tab::Games, Sort::Name).size() == 1);
        assert(Select(games, packages, Tab::Updates, Sort::Name).size() == 2);
        assert(Select(games, packages, Tab::Dlc, Sort::Name).size() == 1);
        assert(Select(games, packages, Tab::All, Sort::Name).size() == 4);
    } else if (test == "ordering") {
        const std::vector<Package> packages = {
            ParsePackage("zebra [0100AABBCC000000].nsp"),
            ParsePackage("Alpha [0100AABBCC002000].nsp"),
            ParsePackage("middle [0100AABBCC004000].nsp"),
        };
        const auto games = Group(packages, {50, 10, 20});
        assert(Select(games, packages, Tab::All, Sort::Name) == std::vector<std::size_t>({1, 2, 0}));
        assert(Select(games, packages, Tab::All, Sort::Size) == std::vector<std::size_t>({0, 2, 1}));
        assert(Select(games, packages, Tab::All, Sort::Newest) == std::vector<std::size_t>({2, 1, 0}));
        assert(Group({packages[0], packages[0]}, {UINT64_MAX, 1})[0].size == UINT64_MAX);
        assert(Group(packages, {}).size() == 3);
    } else if (test == "installed_versions") {
        const auto update = ParsePackage("x [0100AABBCC002800][v65536].nsp");
        assert(IsInstalled(update, ParsePackage("renamed [0100AABBCC002800][v131072].nsz")));
        assert(!IsInstalled(update, ParsePackage("x [0100AABBCC002800][v0].nsp")));
        assert(!IsInstalled(update, ParsePackage("x [0100AABBCC002800].nsp")));
        assert(!IsInstalled(update, ParsePackage("x [0100AABBCC002000][v65536].nsp")));
        assert(!IsInstalled(ParsePackage("Same.nsp"), ParsePackage("Same.nsp")));
        assert(IsInstalled(ParsePackage("x [0100AABBCC002000][v0].nsp"), ParsePackage("x [0100AABBCC002000].xci")));
        assert(!IsInstalled(ParsePackage("x [0100AABBCC003001].nsp"), ParsePackage("x [0100AABBCC003002].nsp")));
    } else if (test == "queue") {
        struct Item { std::string url, key; };
        DownloadQueue<Item> queue;
        Item item{"first", "file"};
        assert(queue.Add(item));
        item.url = "changed";
        assert(queue.Front().url == "first"); // copies survive changes to the catalog
        assert(!queue.Add({"first", "different"}));
        assert(!queue.Add({"mirror", "file"}));
        for (int i = 1; i < 8; ++i)
            assert(queue.Add({std::to_string(i), std::to_string(i)}));
        assert(!queue.Add({"overflow", "overflow"}));
        queue.Remove(3); // cancelling a waiting item preserves the running head
        assert(queue.Front().url == "first" && queue.Items().size() == 7);
        queue.Remove(0); // completion/cancellation advances exactly one item
        assert(queue.Front().url == "1");
        queue.Remove(999);
        while (!queue.Empty()) queue.Remove(0);
        assert(queue.Add({"first", "file"})); // failed/cancelled items can be retried
    } else if (test == "lazy_traversal") {
        Traversal traversal("https://host/index");
        assert(traversal.HasNext());
        const auto root = *traversal.Next();
        assert(!traversal.HasNext());
        const auto index = ParseIndex(R"({"directories":["one","two","one","index"]})", root.url);
        traversal.Follow(root, index);
        assert(traversal.HasNext());
        auto next_page = std::move(traversal);
        assert(next_page.Next()->url == "https://host/one");
        assert(next_page.HasNext());
        assert(next_page.Next()->url == "https://host/two");
        assert(!next_page.HasNext());
    } else { assert(false); }
}
"""


class SourceCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-catalog-")
        cls.addClassCleanup(cls.temp.cleanup)
        driver = Path(cls.temp.name) / "driver.cpp"
        driver.write_text(DRIVER)
        cls.binary = Path(cls.temp.name) / "driver"
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                str(driver),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )


def make_test(case):
    def test(self):
        subprocess.run([str(self.binary), case], check=True, timeout=10)

    return test


for case in (
    "metadata",
    "malformed_and_fallback",
    "group_and_tabs",
    "ordering",
    "installed_versions",
    "queue",
    "lazy_traversal",
):
    setattr(SourceCatalogTests, "test_" + case, make_test(case))


if __name__ == "__main__":
    unittest.main()
