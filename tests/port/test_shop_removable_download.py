# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source contracts for Shop downloads that land on a removable drive.

The UWP application cannot be built or run here, so these pin the structure that fixed two bugs:
a finished download on the USB drive never got its real name (the plain std::filesystem rename is
refused there), and the Shop showed no progress while a download ran.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UWP = ROOT / "src/eden_uwp"
SHOP = (UWP / "ui/sources_screen.cpp").read_text()
DOWNLOAD = (UWP / "game_download.cpp").read_text()
LIBRARY = (UWP / "ui/library.cpp").read_text()
LIBRARY_SCREEN = (UWP / "ui/library_screen.cpp").read_text()
COMMON_FS = (ROOT / "src/common/fs/fs.cpp").read_text()


def body(source, signature):
    """The text between the braces of the function that starts at `signature`."""
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


def in_order(text, *needles):
    positions = [text.index(needle) for needle in needles]
    return positions == sorted(positions)


class FinishingADownloadOnRemovableStorage(unittest.TestCase):
    def test_the_worker_publishes_through_the_fallback_chain(self):
        worker = body(SHOP, "void StartDownload(")
        self.assertIn("PublishDownload(target, complete, reason)", worker)
        # The plain rename was the step that failed on the drive.
        self.assertNotIn("fs::rename(target, complete", worker)
        self.assertNotRegex(worker, r"fs::rename\(\s*target")

    def test_publish_tries_plain_then_broker_win32_then_storage_file(self):
        publish = body(DOWNLOAD, "bool PublishDownload(")
        self.assertTrue(
            in_order(
                publish,
                "std::filesystem::rename(partial, complete, ec)",
                "MoveFileFromAppW(partial.c_str(), complete.c_str())",
                "StorageFile::GetFileFromPathAsync(partial.native())",
                "RenameAsync(complete.filename().native()",
            )
        )
        self.assertIn("NameCollisionOption::ReplaceExisting", publish)
        # A file already under the final name is cleared for the Win32 call, which never replaces.
        self.assertIn("FS::RemoveFile(complete)", publish)

    def test_every_step_reports_its_real_error(self):
        publish = body(DOWNLOAD, "bool PublishDownload(")
        self.assertIn("ec.message()", publish)
        self.assertIn("GetLastError()", publish)
        self.assertIn("HRESULT 0x{:08x}", publish)
        self.assertIn("failure.message()", publish)

    def test_a_failed_download_is_never_logged_bare(self):
        worker = body(SHOP, "void StartDownload(")
        self.assertIn('" failed: " + failure', worker)
        self.assertNotRegex(worker, r'\? " cancelled"\s*:\s*" failed"\)')
        self.assertIn('failure = "cannot rename " + Utf8(target.wstring()) + " (" + reason + ")"', worker)
        self.assertIn('"SOURCES_DOWNLOAD saved "', worker)

    def test_a_complete_partial_on_the_other_drive_is_reused_not_downloaded_again(self):
        download = body(DOWNLOAD, "bool DownloadFile(")
        self.assertIn("drives.push_back({DownloadedSize(candidate), FreeSpace(folder)});", download)
        self.assertIn("have = drives[*pick].have;", download)
        self.assertTrue(
            in_order(download, "have = drives[*pick].have;", "if (have == total)", "Transfer(client"))
        # DownloadedSize goes through Common::FS, which has the broker fallback (std::filesystem
        # does not see files on the drive).
        size = body(DOWNLOAD, "std::uint64_t DownloadedSize(")
        self.assertIn("FS::IsFile(file)", size)
        self.assertIn("FS::GetSize(file)", size)

    def test_the_authenticated_path_reaches_the_drive_through_the_broker_too(self):
        shop = body(SHOP, "bool DownloadShopPackage(")
        for plain in ("std::ofstream", "fs::exists(", "fs::file_size(", "fs::create_directories("):
            self.assertNotIn(plain, shop)
        self.assertIn("Common::FS::IOFile out(", shop)
        self.assertIn("Common::FS::IsFile(candidate)", shop)

    def test_conversion_and_the_nsz_delete_keep_the_broker_fallback(self):
        convert = body(LIBRARY, "std::string ConvertNsz(")
        self.assertIn("Common::FS::RenameFile(partial, nsp_path)", convert)
        self.assertNotRegex(convert, r"\bfs::(rename|remove)\(")
        compressed = body(LIBRARY, "void LibraryScan::ConvertCompressed(")
        self.assertIn("Common::FS::RemoveFile(nsz)", compressed)
        self.assertNotRegex(compressed, r"\bfs::rename\(")
        rename = body(COMMON_FS, "bool RenameFile(")
        self.assertIn("MoveFileFromAppW", rename)
        remove = body(COMMON_FS, "bool RemoveFile(")
        self.assertIn("DeleteFileFromAppW", remove)


class ShopProgressWhileDownloading(unittest.TestCase):
    def test_the_download_runs_on_its_own_thread(self):
        start = body(SHOP, "void StartDownload(")
        self.assertIn("download_thread_ = std::thread(", start)
        self.assertEqual(SHOP.count("DownloadShopPackage("), 2)  # the definition and this worker
        self.assertIn("DownloadShopPackage(package, target, progress, cancel_)", start)

    def test_nothing_the_frame_runs_blocks_on_the_download(self):
        for signature in (
            "void TickShop(",
            "void DrawShop(",
            "void Draw(Clock::time_point now)",
            "void DrawQueue(",
            "void DrawProgress(",
            "void DrawShopDownloadStatus(",
        ):
            text = body(SHOP, signature)
            for blocking in ("AwaitBounded(", ".get()", "wait_for(", ".join()", "sleep_for("):
                self.assertNotIn(blocking, text, f"{signature} {blocking}")
        update = body(SHOP, "void UpdateDownload(")
        self.assertTrue(
            in_order(update, "== Outcome::Pending", "download_thread_.join()"),
            "the join may only run after the worker has stored its outcome",
        )

    def test_the_library_loop_draws_every_frame(self):
        run = body(LIBRARY_SCREEN, "std::string Run()")
        self.assertTrue(in_order(run, "Update(now);", "renderer_.BeginFrame();", "Draw(now);", "renderer_.EndFrame();"))
        self.assertNotRegex(run, r"if\s*\([^)]*\)\s*\{?\s*Draw\(now\)")

    def test_the_shop_draws_the_progress_outside_the_queue_page(self):
        draw = body(SHOP, "void Draw(Clock::time_point now)")
        self.assertRegex(draw, r"if \(shop_ && section_ != 5\)\s*DrawShopDownloadStatus\(\);")
        status = body(SHOP, "void DrawShopDownloadStatus(")
        self.assertIn("bytes_done_.load()", status)
        self.assertIn("rate_.load()", status)
        self.assertIn("if (queue_.Empty())", status)

    def test_the_counters_refresh_within_500_ms(self):
        self.assertIn("kRateWindow = std::chrono::milliseconds(500)", SHOP)
        worker = body(SHOP, "void StartDownload(")
        progress = worker[worker.index("const auto progress") : worker.index("// Keep incomplete packages")]
        # Bytes are stored on every call, before the rate window check.
        self.assertTrue(in_order(progress, "bytes_done_.store(have)", "at - sample_at >= kRateWindow"))
        self.assertIn("rate_.store(", progress)
        # The transfer reports after every chunk it reads.
        transfer = body(DOWNLOAD, "void Transfer(")
        self.assertRegex(transfer, r"have \+= read\.Length\(\);\s*if \(progress\) \{\s*progress\(have, total\);")


if __name__ == "__main__":
    unittest.main()
