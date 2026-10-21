# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production NRO metadata constructor with bounded, counted VFS reads."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using u32_le = u32;
using u64_le = u64;
#define JOIN_I(a,b) a##b
#define JOIN(a,b) JOIN_I(a,b)
#define INSERT_PADDING_BYTES(n) std::byte JOIN(padding_,__LINE__)[n]
#define LOG_WARNING(...) ((void)0)
namespace Common {
constexpr u32 MakeMagic(char a,char b,char c,char d) {
    return u32(a)|(u32(b)<<8)|(u32(c)<<16)|(u32(d)<<24);
}
}
namespace FileSys {
struct VfsFile {
    std::vector<u8> bytes;
    std::size_t reads = 0;
    std::size_t GetSize() const { return bytes.size(); }
    template<class T> std::size_t ReadObject(T* out,std::size_t offset=0) {
        if (offset > bytes.size() || sizeof(T) > bytes.size()-offset) return 0;
        std::memcpy(out,bytes.data()+offset,sizeof(T)); return sizeof(T);
    }
    std::vector<u8> ReadBytes(std::size_t size,std::size_t offset) {
        ++reads;
        // Fail deterministically before an allocation, even against the vulnerable constructor.
        if (size > 1024*1024 || offset > bytes.size() || size > bytes.size()-offset)
            throw std::runtime_error("unbounded asset read");
        return {bytes.begin()+offset,bytes.begin()+offset+size};
    }
};
using VirtualFile = std::shared_ptr<VfsFile>;
struct OffsetVfsFile : VfsFile {
    u64 size, offset;
    OffsetVfsFile(VirtualFile,u64 size_,u64 offset_,const char*) : size(size_),offset(offset_) {}
};
struct RawNACP { u8 data[0x4000]; };
struct NACP { explicit NACP(std::shared_ptr<OffsetVfsFile>) {} };
}
namespace Loader {
enum class FileType { NRO, Error };
struct AppLoader {
    explicit AppLoader(FileSys::VirtualFile source) : file(std::move(source)) {}
    FileSys::VirtualFile file;
};
struct AppLoader_NRO : AppLoader {
    explicit AppLoader_NRO(FileSys::VirtualFile);
    static FileType IdentifyType(const FileSys::VirtualFile&);
    std::unique_ptr<FileSys::NACP> nacp;
    FileSys::VirtualFile romfs;
    std::vector<u8> icon_data;
};
// PRODUCTION
}
using namespace Loader;
FileSys::VirtualFile Fixture(AssetHeader assets, std::size_t payload_size=0) {
    auto file = std::make_shared<FileSys::VfsFile>();
    NroHeader header{};
    static_assert(sizeof(header) == 0x80);
    static_assert(sizeof(assets) == 0x38);
    header.magic = Common::MakeMagic('N','R','O','0');
    header.file_size = sizeof(header);
    assets.magic = Common::MakeMagic('A','S','E','T');
    file->bytes.resize(sizeof(header)+sizeof(assets)+payload_size,0xab);
    std::memcpy(file->bytes.data(),&header,sizeof(header));
    std::memcpy(file->bytes.data()+sizeof(header),&assets,sizeof(assets));
    return file;
}
int main() {
    AssetHeader valid{};
    valid.icon = {sizeof(AssetHeader),4};
    valid.nacp = {sizeof(AssetHeader)+4,sizeof(FileSys::RawNACP)};
    valid.romfs = {sizeof(AssetHeader)+4+sizeof(FileSys::RawNACP),8};
    auto file = Fixture(valid,4+sizeof(FileSys::RawNACP)+8);
    assert(AppLoader_NRO::IdentifyType(file) == FileType::NRO);
    AppLoader_NRO good(file);
    assert(good.nacp && good.romfs && good.icon_data == std::vector<u8>(4,0xab));
    assert(file->reads == 1);
    // The reported 184-byte/8-GiB icon case, overflows, short reads, and header overlap.
    for (const auto& icon : std::vector<AssetSection>{
             {sizeof(AssetHeader),8ull*1024*1024*1024},
             {sizeof(AssetHeader),std::numeric_limits<u64>::max()},
             {std::numeric_limits<u64>::max(),2}, {sizeof(AssetHeader),5}, {8,4}}) {
        auto assets = valid;
        assets.icon = icon;
        file = Fixture(assets, icon.size == 5 ? 4 : 0);
        AppLoader_NRO bad(file);
        assert(bad.icon_data.empty() && !bad.nacp && !bad.romfs && file->reads == 0);
    }
    // Even a present oversized icon cannot consume the UWP app's memory budget.
    AssetHeader oversized{};
    oversized.icon = {sizeof(AssetHeader),1024*1024+1};
    file = Fixture(oversized,1024*1024+1);
    AppLoader_NRO capped(file);
    assert(capped.icon_data.empty() && file->reads == 0);
    oversized.icon.size = 1024*1024;
    file = Fixture(oversized,1024*1024);
    AppLoader_NRO boundary(file);
    assert(boundary.icon_data.size() == 1024*1024 && file->reads == 1);
    // Ignore invalid optional assets independently, preserving valid title metadata.
    auto assets = valid;
    assets.icon.size = std::numeric_limits<u64>::max();
    file = Fixture(assets,4+sizeof(FileSys::RawNACP)+8);
    AppLoader_NRO partial(file);
    assert(partial.nacp && partial.romfs && partial.icon_data.empty() && file->reads == 0);
    for (const auto& nacp : std::vector<AssetSection>{
             {sizeof(AssetHeader),1}, {std::numeric_limits<u64>::max(),0x4000},
             {sizeof(AssetHeader),std::numeric_limits<u64>::max()}}) {
        assets = {};
        assets.nacp = nacp;
        file = Fixture(assets,sizeof(FileSys::RawNACP));
        AppLoader_NRO bad(file);
        assert(!bad.nacp);
    }
    assets = {};
    assets.romfs = {std::numeric_limits<u64>::max(),1};
    file = Fixture(assets);
    assert(!AppLoader_NRO(file).romfs);
    for (const auto declared_size : {u32{0},std::numeric_limits<u32>::max()}) {
        file = Fixture(valid,4+sizeof(FileSys::RawNACP)+8);
        std::memcpy(file->bytes.data()+0x18,&declared_size,sizeof(declared_size));
        AppLoader_NRO bad(file);
        assert(!bad.nacp && !bad.romfs && bad.icon_data.empty() && file->reads == 0);
    }
    file = Fixture(valid);
    file->bytes.resize(sizeof(NroHeader)); // NRO without optional ASET metadata.
    AppLoader_NRO no_assets(file);
    assert(no_assets.icon_data.empty() && !no_assets.nacp && !no_assets.romfs);
    file->bytes.resize(16);
    assert(AppLoader_NRO::IdentifyType(file) == FileType::Error);
    assert(AppLoader_NRO(file).icon_data.empty());
    file = Fixture(valid,4+sizeof(FileSys::RawNACP)+8);
    file->bytes[sizeof(NroHeader)] = 0; // An unrecognized asset header is optional.
    AppLoader_NRO unknown_assets(file);
    assert(!unknown_assets.nacp && !unknown_assets.romfs && file->reads == 0);
}
"""


class NroMetadata(unittest.TestCase):
    def test_production_constructor_rejects_unbounded_or_truncated_assets(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "A C++ compiler is required for NRO regression tests")
        source = (ROOT / "src/core/loader/nro.cpp").read_text(encoding="utf-8")
        production = "\n".join(
            function(source, "struct " + name) + ";"
            for name in ("NroSegmentHeader", "NroHeader", "AssetSection", "AssetHeader")
        )
        production += "\n" + function(source, "AppLoader_NRO::AppLoader_NRO")
        production += "\n" + function(source, "FileType AppLoader_NRO::IdentifyType")
        with tempfile.TemporaryDirectory(prefix="nro-metadata-") as temp:
            cpp, binary = Path(temp) / "metadata.cpp", Path(temp) / "metadata"
            cpp.write_text(PRELUDE.replace("// PRODUCTION", production), encoding="utf-8")
            subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
