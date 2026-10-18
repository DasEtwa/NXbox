# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic firmware inputs only. Python XEX reference uses the system AES-ECB primitive."""

import shlex
import struct
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEY = bytes(range(32))  # Public synthetic fixture, never a console key.
DRIVER = r"""
#include "frontend_common/firmware_sources.h"
#include <openssl/evp.h>
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace FirmwareManager::Sources;
struct FileWriter : Writer {
    std::ofstream f;
    FileWriter(const fs::path& p) : f(p, std::ios::binary) { if (!f) throw std::runtime_error("open"); }
    void Write(View b) override { f.write(reinterpret_cast<const char*>(b.data()), b.size()); if (!f) throw std::runtime_error("write"); }
    void Finish() override { f.close(); if (!f) throw std::runtime_error("close"); }
};
int main(int argc, char** argv) {
    assert(argc == 5);
    IO io;
    io.list = [](const fs::path& p) {
        std::vector<Entry> result;
        if (!fs::exists(p)) return result;
        if (fs::is_symlink(p)) throw std::runtime_error("symlink root");
        for (const auto& e : fs::directory_iterator(p)) {
            const bool reparse = e.is_symlink();
            result.push_back({e.path(), !reparse && e.is_directory(), reparse,
                !reparse && e.is_regular_file() ? e.file_size() : 0,
                !reparse ? static_cast<uint64_t>(e.last_write_time().time_since_epoch().count()) : 0});
        }
        return result;
    };
    io.open = [](const fs::path& p) {
        auto f = std::make_shared<std::ifstream>(p, std::ios::binary);
        if (!*f) throw std::runtime_error("open");
        return Reader{fs::file_size(p), [f](uint64_t off, Bytes b) {
            f->seekg(off); f->read(reinterpret_cast<char*>(b.data()), b.size());
            if (!*f) throw std::runtime_error("read");
        }};
    };
    io.create = [](const fs::path& p) { return std::make_unique<FileWriter>(p); };
    io.mkdir = [](const fs::path& p) { if (!fs::create_directory(p)) throw std::runtime_error("mkdir"); };
    io.remove_tree = [](const fs::path& p) { fs::remove_all(p); };
    io.log = [](const std::string& s) { std::cout << s << '\n'; };
    const fs::path input(argv[2]), output(argv[3]);
    const std::string mode(argv[1]);
    if (mode == "discover") {
        const auto sources = Discover(io, {input / "local" / "firmware", input / "firmware"}, {input});
        for (const auto& s : sources) {
            if (!s.parts.empty()) {
                const auto fingerprint = Fingerprint(s);
                auto changed = s;
                ++changed.parts.front().size;
                assert(Fingerprint(changed) != fingerprint);
                changed = s;
                ++changed.parts.front().modified;
                assert(Fingerprint(changed) != fingerprint);
                changed = s;
                changed.parts.front().path += ".moved";
                assert(Fingerprint(changed) != fingerprint);
                assert(Fingerprint(s) == fingerprint);
            }
            std::cout << static_cast<int>(s.kind) << ':' << s.path.lexically_relative(input).generic_string()
                      << ':' << s.parts.size() << ':' << s.archive_folder.generic_string() << '\n';
        }
        return 0;
    }
    Source source{mode == "folder" ? Kind::Folder : mode == "zip" ? Kind::Zip : Kind::Nand, input, {}, {}};
    if (mode == "parts") {
        source.parts = io.list(input);
        std::sort(source.parts.begin(), source.parts.end(), [](auto& a, auto& b) { return a.path < b.path; });
    } else if (mode == "nand") {
        for (const auto& e : io.list(input.parent_path())) if (e.path == input) source.parts.push_back(e);
    }
    std::array<unsigned char, 32> key{};
    for (size_t i = 0; i < key.size(); ++i) key[i] = i;
    Decrypt decrypt = [key](uint64_t sector, View in, Bytes out) {
        std::array<unsigned char, 16> iv{};
        for (int i = 15; i >= 0; --i) { iv[i] = sector & 255; sector >>= 8; }
        auto* c = EVP_CIPHER_CTX_new(); assert(c);
        assert(EVP_DecryptInit_ex(c, EVP_aes_128_xts(), nullptr, key.data(), iv.data()));
        assert(EVP_CIPHER_CTX_set_padding(c, 0));
        int n = 0, tail = 0;
        assert(EVP_DecryptUpdate(c, out.data(), &n, in.data(), in.size()));
        assert(EVP_DecryptFinal_ex(c, out.data() + n, &tail));
        assert(static_cast<size_t>(n + tail) == in.size()); EVP_CIPHER_CTX_free(c);
    };
    bool failed = false;
    try {
        auto files = Extract(io, source, output, decrypt);
        for (const auto& p : files) {
            if (p.parent_path() != output) fs::copy_file(p, output / p.filename());
        }
    } catch (const std::exception& e) {
        failed = true; std::cout << "rejected: " << e.what() << '\n';
        io.remove_tree(output);
    }
    assert(failed == (std::string(argv[4]) == "reject"));
}
"""


def aes_ecb(data, key):
    return subprocess.run(
        ["openssl", "enc", "-aes-128-ecb", "-nopad", "-K", key.hex()],
        input=data,
        capture_output=True,
        check=True,
    ).stdout


def encrypt_xts(data):
    """Independent IEEE 1619 XEX/GF reference with Nintendo big-endian sector IVs."""
    assert len(data) % 0x4000 == 0
    ivs = b"".join(i.to_bytes(16, "big") for i in range(len(data) // 0x4000))
    initial = aes_ecb(ivs, KEY[16:])
    tweaks = bytearray()
    for sector in range(len(ivs) // 16):
        t = int.from_bytes(initial[sector * 16 : sector * 16 + 16], "little")
        for _ in range(0x4000 // 16):
            tweaks.extend(t.to_bytes(16, "little"))
            carry = t >> 127
            t = ((t << 1) & ((1 << 128) - 1)) ^ (0x87 if carry else 0)
    whitened = bytes(a ^ b for a, b in zip(data, tweaks))
    encoded = aes_ecb(whitened, KEY[:16])
    return bytes(a ^ b for a, b in zip(encoded, tweaks))


def fat_entry(name, cluster, size=0, directory=False):
    """Build a valid VFAT LFN + short entry, including ordinal and alias checksum."""
    alias = b"FIXTURE NCA"
    checksum = 0
    for ch in alias:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + ch) & 255
    units = [ord(ch) for ch in name] + [0]
    units += [0xFFFF] * ((-len(units)) % 13)
    result = bytearray()
    for i in reversed(range(len(units) // 13)):
        e = bytearray(32)
        e[0] = i + 1 | (0x40 if i == len(units) // 13 - 1 else 0)
        e[11], e[13] = 15, checksum
        for pos, unit in zip(
            (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30), units[i * 13 : i * 13 + 13]
        ):
            struct.pack_into("<H", e, pos, unit)
        result.extend(e)
    e = bytearray(32)
    e[:11] = alias
    e[11] = 16 if directory else 32
    struct.pack_into("<H", e, 20, cluster >> 16)
    struct.pack_into("<H", e, 26, cluster & 65535)
    struct.pack_into("<I", e, 28, size)
    return result + e


def nand_fixture():
    # Deliberately tiny FAT32 geometry (no real firmware). File data crosses XTS sectors.
    fat = bytearray(0x10000)
    struct.pack_into("<H", fat, 11, 512)
    fat[13] = 1
    struct.pack_into("<H", fat, 14, 1)
    fat[16] = 1
    struct.pack_into("<I", fat, 32, len(fat) // 512)
    struct.pack_into("<I", fat, 36, 1)
    struct.pack_into("<I", fat, 44, 2)
    struct.pack_into("<H", fat, 510, 0xAA55)

    def chain(c, payload):
        count = (len(payload) + 511) // 512
        for i in range(count):
            struct.pack_into(
                "<I", fat, 512 + (c + i) * 4, c + i + 1 if i + 1 < count else 0xFFFFFFF
            )
            offset = 1024 + (c + i - 2) * 512
            fat[offset : offset + min(512, len(payload) - i * 512)] = payload[
                i * 512 : (i + 1) * 512
            ]

    plain = bytes(range(256)) * 101
    chunks = (b"first chunk\0" * 73, b"second chunk" * 103)
    chain(2, fat_entry("Contents", 3, directory=True) + fat_entry("á unrelated", 90, 1))
    chain(3, fat_entry("registered", 4, directory=True))
    chain(
        4,
        fat_entry("0123456789abcdef.cnmt.nca", 10, len(plain))
        + fat_entry("fedcba9876543210.nca", 5, directory=True),
    )
    chain(5, fat_entry("00", 70, len(chunks[0])) + fat_entry("01", 80, len(chunks[1])))
    chain(10, plain)
    chain(70, chunks[0])
    chain(80, chunks[1])
    encrypted = encrypt_xts(fat)
    image = bytearray(0x4000) + encrypted
    image[512:520] = b"EFI PART"
    struct.pack_into("<I", image, 512 + 12, 92)
    struct.pack_into("<QII", image, 512 + 72, 2, 1, 128)
    image[1024:1040] = bytes(range(1, 17))
    struct.pack_into("<QQ", image, 1024 + 32, 0x4000 // 512, len(image) // 512 - 1)
    name = "SYSTEM".encode("utf-16le")
    image[1024 + 56 : 1024 + 56 + len(name)] = name
    return bytes(image), {
        "0123456789abcdef.cnmt.nca": plain,
        "fedcba9876543210.nca": b"".join(chunks),
    }


class FirmwareSourcesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxbox-firmware-build-")
        root = Path(cls.build.name)
        driver = root / "driver.cpp"
        driver.write_text(DRIVER)
        cls.binary = root / "driver"
        flags = shlex.split(
            subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True)
        )
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
                str(ROOT / "src/frontend_common/firmware_sources.cpp"),
                "-lz",
                *flags,
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        cls.image, cls.expected = nand_fixture()

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nxbox-firmware-source-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_driver(self, mode, source, reject=False):
        output = self.root / "out"
        result = subprocess.run(
            [str(self.binary), mode, str(source), str(output), "reject" if reject else "ok"],
            check=True,
            capture_output=True,
            text=True,
            timeout=20,
        )
        return output, result.stdout

    def assert_files(self, output, expected):
        self.assertEqual({p.name: p.read_bytes() for p in output.iterdir()}, expected)

    def test_recursive_cnmt_and_chunks(self):
        source = self.root / "firmware"
        nested = source / "a/b/c/d/e/f/g/h"
        nested.mkdir(parents=True)
        (nested / "deep.cnmt.nca").write_bytes(b"metadata")
        chunk = source / "hash.nca"
        chunk.mkdir()
        (chunk / "00").write_bytes(b"zero\x00")
        (chunk / "01").write_bytes(b"one\xff")
        (source / "link.nca").symlink_to(nested / "deep.cnmt.nca")
        (source / "linked").symlink_to(nested, target_is_directory=True)
        (nested / "too-deep").mkdir()
        (nested / "too-deep/ignored.nca").write_bytes(b"ignored")
        output, _ = self.run_driver("folder", source)
        self.assert_files(output, {"deep.cnmt.nca": b"metadata", "hash.nca": b"zero\x00one\xff"})

    def test_missing_chunk_rejected(self):
        source = self.root / "firmware"
        (source / "hash.nca").mkdir(parents=True)
        (source / "hash.nca/01").write_bytes(b"bad")
        output, _ = self.run_driver("folder", source, reject=True)
        self.assertFalse(output.exists())
        self.assertTrue((source / "hash.nca/01").exists())

    def test_stored_deflate_and_zip64(self):
        for compression, zip64 in (
            (zipfile.ZIP_STORED, False),
            (zipfile.ZIP_DEFLATED, False),
            (zipfile.ZIP_DEFLATED, True),
        ):
            with self.subTest(compression=compression, zip64=zip64):
                archive = self.root / "firmware.zip"
                with zipfile.ZipFile(archive, "w", compression=compression) as z:
                    for name, data in {
                        "a/b/file.cnmt.nca": b"content" * 30000,
                        "a/hash.nca/00": b"zero",
                        "a/hash.nca/01": b"one",
                    }.items():
                        if zip64:
                            with z.open(name, "w", force_zip64=True) as f:
                                f.write(data)
                        else:
                            z.writestr(name, data)
                if zip64:
                    # Also force ZIP64 central directory + EOCD/locator on a tiny archive.
                    with zipfile.ZipFile(archive) as z:
                        entries = [(e.filename, z.read(e)) for e in z.infolist()]
                    old = zipfile.ZIP64_LIMIT
                    try:
                        zipfile.ZIP64_LIMIT = 1
                        with zipfile.ZipFile(archive, "w", compression=compression) as z:
                            for name, data in entries:
                                z.writestr(name, data)
                    finally:
                        zipfile.ZIP64_LIMIT = old
                output, log = self.run_driver("zip", archive)
                self.assertIn("FIRMWARE zip_extract", log)
                self.assert_files(
                    output, {"file.cnmt.nca": b"content" * 30000, "hash.nca": b"zeroone"}
                )
                self.assertTrue(archive.exists())
                for p in output.iterdir():
                    p.unlink()
                output.rmdir()

    def test_zip_traversal_and_crc_rejected(self):
        archive = self.root / "bad.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("../escape.nca", b"bad")
        output, _ = self.run_driver("zip", archive, reject=True)
        self.assertFalse(output.exists())
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("file.nca", b"PAYLOAD")
        archive.write_bytes(archive.read_bytes().replace(b"PAYLOAD", b"CORRUPT", 1))
        self.run_driver("zip", archive, reject=True)

    def test_nand_xts_fat_files_and_chunks(self):
        path = self.root / "rawnand.bin"
        path.write_bytes(self.image)
        output, log = self.run_driver("nand", path)
        self.assertIn("FIRMWARE nand_extract count=2", log)
        self.assert_files(output, self.expected)
        self.assertEqual(path.read_bytes(), self.image)

    def test_split_rawnand_and_emummc(self):
        for prefix in ("rawnand.bin.", ""):
            with self.subTest(prefix=prefix):
                source = self.root / ("raw" if prefix else "eMMC")
                source.mkdir()
                # Split through an XTS sector and a GPT entry; Joined must support arbitrary reads.
                cut = 1083
                (source / f"{prefix}00").write_bytes(self.image[:cut])
                (source / f"{prefix}01").write_bytes(self.image[cut:])
                output, _ = self.run_driver("parts", source)
                self.assert_files(output, self.expected)
                for p in output.iterdir():
                    p.unlink()
                output.rmdir()

    def test_corrupt_gpt_and_encrypted_fat_rejected(self):
        path = self.root / "rawnand.bin"
        corrupt = bytearray(self.image)
        corrupt[512] = 0
        path.write_bytes(corrupt)
        self.run_driver("nand", path, reject=True)
        corrupt = bytearray(self.image)
        corrupt[0x4000 + 510] ^= 1
        path.write_bytes(corrupt)
        self.run_driver("nand", path, reject=True)

    def test_discovery_priority_protection_and_split_validation(self):
        (self.root / "local/firmware").mkdir(parents=True)
        (self.root / "local/firmware/a.nca").write_bytes(b"local")
        (self.root / "firmware").mkdir()
        (self.root / "firmware/rawnand.bin").write_bytes(b"raw")
        (self.root / "firmware/keep.nca").write_bytes(b"loose beside NAND")
        (self.root / "firmware/unsupported.rar").write_bytes(b"rar")
        emu = self.root / "NXbox/emuMMC/SD00/eMMC"
        emu.mkdir(parents=True)
        (emu / "00").write_bytes(b"first")
        (emu / "01").write_bytes(b"second")
        (emu / "boot0").write_bytes(b"ignored")
        (self.root / "root.nca").write_bytes(b"root")
        _, log = self.run_driver("discover", self.root)
        sources = [line for line in log.splitlines() if line[:1].isdigit()]
        self.assertTrue(sources[0].startswith("0:local/firmware:"))
        self.assertIn("0:firmware:1:", sources)  # NAND-containing folder must never be renamed.
        self.assertIn("2:firmware/rawnand.bin:1:", sources)
        self.assertIn("2:NXbox/emuMMC/SD00/eMMC/00:2:", sources)
        self.assertIn("NxHelper on a computer can convert it", log)
        (emu / "01").rename(emu / "02")
        _, log = self.run_driver("discover", self.root)
        self.assertIn("invalid split sequence", log)
        self.assertNotIn("2:NXbox/emuMMC", log)


if __name__ == "__main__":
    unittest.main()
