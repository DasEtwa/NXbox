# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixture-backed coverage of Tinfoil locations and autoindex formats."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).with_name("fixtures")
DRIVER = r"""
#include <cassert>
#include <fstream>
#include <iterator>
#include "eden_uwp/ui/tinfoil_index.h"
using namespace EdenXbox::Tinfoil;
int main(int argc, char** argv) {
    assert(argc == 3);
    std::ifstream file(argv[2], std::ios::binary);
    assert(file);
    const std::string body((std::istreambuf_iterator<char>(file)), {});
    const auto index = ParseIndex(body, "https://host/shop/");
    const std::string test = argv[1];
    if (test == "relative_fragment_size") {
        assert(index.status == Status::Ok && index.files.size() == 1);
        assert(index.files[0].url == "https://host/games/base.nsp");
        assert(index.files[0].name == "Example [0100000000000000] [v0].nsp");
        assert(index.files[0].size == 12345 && index.files[0].kind == Kind::Game);
    } else if (test == "directories") {
        assert(index.directories.size() == 2);
        assert(index.directories[0] == "https://host/shop/child/");
        Traversal traversal("https://host/shop/");
        const auto root = traversal.Next();
        traversal.Follow(*root, index);
        assert(traversal.Next()->depth == 1);
    } else if (test == "headers_referrer") {
        assert(index.headers.size() == 3);
        assert(index.headers[0] == std::make_pair(std::string("Referer"), std::string("https://host/welcome")));
        Traversal traversal("https://host/shop/");
        const auto root = traversal.Next();
        traversal.Follow(*root, index);
        assert(traversal.Next()->headers == index.headers);
    } else if (test == "success") {
        assert(index.success == "Welcome to the fixture shop");
    } else if (test == "locations") {
        assert(index.locations.size() == 4);
        assert(index.locations[0] == "https://second.example/shop/");
        assert(index.locations[1] == "gdrive:public-folder-id");
        assert(index.locations[3] == "https://host/another/");
        Traversal traversal("https://host/shop/");
        for (const auto& location : index.locations)
            traversal.AddRoot(location);
        assert(traversal.Next()->depth == 0);
        assert(traversal.Next()->url == index.locations[0]);
        for (int i = 0; i < 100; ++i) traversal.AddRoot("https://host/" + std::to_string(i));
        int count = 2;
        while (traversal.Next()) ++count;
        assert(count == kMaxIndexes);
    } else if (test == "error") {
        assert(index.status == Status::SourceError);
        assert(index.error == "Shop closed for maintenance");
        assert(index.files.empty() && index.directories.empty());
    } else if (test == "encrypted") {
        assert(index.status == Status::Unsupported && index.unsupported == Unsupported::Encrypted);
    } else if (test == "apache") {
        assert(index.status == Status::Ok && index.files.size() == 1 && index.directories.size() == 1);
        assert(index.files[0].name == "Game [0100000000000000].nsp");
        assert(index.directories[0] == "https://host/shop/child/");
    } else if (test == "nginx") {
        assert(index.status == Status::Ok && index.files.size() == 1 && index.directories.size() == 1);
        assert(index.files[0].url == "https://host/shop/base.nsz");
    } else if (test == "iis") {
        assert(index.status == Status::Ok && index.files.size() == 2 && index.directories.size() == 1);
        assert(index.files[0].url == "https://host/shop/base.xci?token=a&b=2");
        assert(index.files[1].url == "https://host/shop/compressed.XCZ");
    } else if (test == "basic_auth") {
        Headers headers;
        assert(Authenticate("https://user:p%40ss@host/shop/", headers) == "https://host/shop/");
        assert(headers.size() == 1 && headers[0].second == "Basic dXNlcjpwQHNz");
        assert(Authenticate("https://host/plain", headers) == "https://host/plain");
        assert(headers.size() == 1);
        Traversal traversal("https://user:pass@host/shop/");
        const auto root = traversal.Next();
        assert(root->headers.size() == 1 && root->headers[0].second == "Basic dXNlcjpwYXNz");
        const auto basic_index = ParseIndex(R"({"directories":["child/"]})", root->url);
        traversal.Follow(*root, basic_index);
        const auto child = traversal.Next();
        assert(child && child->headers == root->headers);
        Headers custom = {{"Authorization", "Bearer custom"}};
        Authenticate(root->url, custom);
        assert(custom.size() == 1 && custom[0].second == "Bearer custom");
    } else if (test == "ftp") {
        assert(IsLocation("ftp://host/games/") && IsLocation("ftps://host/games/"));
        assert(LocationSupport("ftp://host/games/") == Unsupported::Ftp);
        assert(LocationSupport("FTPS://host/games/") == Unsupported::Ftp);
    } else if (test == "gdrive") {
        assert(IsLocation("gdrive:folder-id"));
        assert(LocationSupport("gdrive:folder-id") == Unsupported::GoogleDrive);
    } else if (test == "1fichier") {
        assert(LocationSupport("https://1fichier.com/?public-link") == Unsupported::OneFichier);
        assert(LocationSupport("https://dl.1fichier.com/token") == Unsupported::OneFichier);
        assert(LocationSupport("https://not1fichier.com/index.json") == Unsupported::None);
        assert(!IsLocation("My shop|https://1fichier.com/?public-link"));
        assert(!IsLocation("ftp://") && !IsLocation("gdrive:"));
    } else {
        assert(false);
    }
}
"""


class TinfoilLocationsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-locations-")
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "locations.cpp"
        source.write_text(DRIVER)
        cls.binary = Path(cls.temp.name) / "locations"
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )


def make_test(case, fixture):
    def test(self):
        subprocess.run([str(self.binary), case, str(FIXTURES / fixture)], check=True, timeout=10)

    return test


for case in (
    "relative_fragment_size",
    "directories",
    "headers_referrer",
    "success",
    "locations",
    "basic_auth",
    "ftp",
    "gdrive",
    "1fichier",
):
    setattr(TinfoilLocationsTests, "test_" + case, make_test(case, "tinfoil_features.json"))
for case, fixture in (
    ("error", "tinfoil_error.json"),
    ("encrypted", "tinfoil_encrypted.txt"),
    ("apache", "tinfoil_apache.html"),
    ("nginx", "tinfoil_nginx.html"),
    ("iis", "tinfoil_iis.html"),
):
    setattr(TinfoilLocationsTests, "test_" + case, make_test(case, fixture))
