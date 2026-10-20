# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production FSP read handler with a bounded, short-reading backend."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/core/hle/service/filesystem/fsp/fs_i_storage.cpp"

HARNESS = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
using u8 = std::uint8_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using Result = int;
constexpr Result ResultUnknown = 9;
namespace FileSys {
constexpr Result ResultInvalidSize = 1;
constexpr Result ResultInvalidOffset = 2;
constexpr Result ResultOutOfRange = 3;
}
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define R_UNLESS(condition, result) do { if (!(condition)) return result; } while (0)
#define R_THROW(result) return result
#define R_SUCCEED() return 0
constexpr int BufferAttr_HipcMapAlias = 4;
constexpr int BufferAttr_HipcMapTransferAllowsNonSecure = 64;
template <int Attr> using OutBuffer = std::span<u8>;
struct Backend {
    u64 size = 64;
    u64 available = 64;
    int calls = 0;
    u64 last_offset = 0;
    u64 GetSize() const { return size; }
    u64 Read(u8* buffer, u64 length, u64 offset) {
        ++calls;
        last_offset = offset;
        const u64 count = std::min(length, available);
        std::fill_n(buffer, count, 0x5a);
        return count;
    }
};
struct IStorage {
    Backend* backend;
    Result Read(OutBuffer<68> out_bytes, s64 offset, s64 length);
};
__PRODUCTION_READ__
int main() {
    Backend backend;
    IStorage storage{&backend};
    std::array<u8, 10> bytes{};
    auto buffer = std::span{bytes}.subspan(1, 8);
    assert(storage.Read(buffer, 3, 8) == 0);
    assert(backend.calls == 1 && backend.last_offset == 3);
    assert(bytes.front() == 0 && bytes.back() == 0 && bytes[1] == 0x5a);

    backend.available = 0;
    assert(storage.Read(buffer, 0, 8) == ResultUnknown);
    backend.available = 7;
    assert(storage.Read(buffer, 0, 8) == ResultUnknown);
    const int calls = backend.calls;
    assert(storage.Read(buffer, 0, 9) == FileSys::ResultInvalidSize);
    assert(storage.Read(buffer, -1, 8) == FileSys::ResultInvalidOffset);
    assert(storage.Read(buffer, 0, -1) == FileSys::ResultInvalidSize);
    assert(storage.Read(buffer, 60, 8) == FileSys::ResultOutOfRange);
    assert(storage.Read(buffer, 65, 0) == FileSys::ResultOutOfRange);
    assert(storage.Read({}, 64, 0) == 0);
    assert(storage.Read(buffer, std::numeric_limits<s64>::max(), 8) ==
           FileSys::ResultOutOfRange);
    assert(backend.calls == calls);

    // No narrowing at the 4 GiB boundary, and a buffer larger than length is allowed.
    backend.size = (u64{1} << 33);
    backend.available = 4;
    assert(storage.Read(buffer, (s64{1} << 32) + 3, 4) == 0);
    assert(backend.last_offset == (u64{1} << 32) + 3);
    assert(bytes.back() == 0);
}
"""


class FspStorageReadTests(unittest.TestCase):
    def test_exact_read_and_ipc_buffer_bounds(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        # Compile the real handler; only IPC/framework types and backend I/O are replaced.
        source = SOURCE.read_text()
        method = "Result IStorage::Read(" + source.split("Result IStorage::Read(", 1)[1]
        method = method.split("\nResult IStorage::GetSize", 1)[0]
        with tempfile.TemporaryDirectory(prefix="nxbox-fsp-read-", dir="/tmp") as temporary:
            driver = Path(temporary) / "read.cpp"
            binary = Path(temporary) / "read"
            driver.write_text(HARNESS.replace("__PRODUCTION_READ__", method))
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(driver),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
