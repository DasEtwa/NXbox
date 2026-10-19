# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pin the Shop's worker-to-UI inventory wiring without requiring the UWP SDK."""

import unittest
from pathlib import Path

from test_shop_removable_download import body, in_order

ROOT = Path(__file__).resolve().parents[2]
SHOP = (ROOT / "src/eden_uwp/ui/sources_screen.cpp").read_text()
USB = (ROOT / "src/eden_uwp/usb_library.cpp").read_text()


class ExternalInstalledInventory(unittest.TestCase):
    def test_external_enumeration_and_metadata_stay_inside_the_mta_worker(self):
        tick = body(SHOP, "void TickShop(")
        worker = body(tick, "storage_ = Spawn<StorageResult>(")
        self.assertEqual(SHOP.count("ListExternalGames()"), 1)
        self.assertIn("for (const auto& file : ListExternalGames())", worker)
        self.assertIn("task.result.external_files.push_back(ReadInstalledFile(file))", worker)
        # Remove the entire worker before checking the UI method.
        ui = tick.replace(worker, "")
        for call in (
            "ListExternalGames()",
            "ReadInstalledFile(",
            "ExternalGameFolders()",
            "FreeSpace(",
        ):
            self.assertNotIn(call, ui)
        spawn = body(SHOP, "std::shared_ptr<Task<Result>> Spawn(")
        self.assertTrue(
            in_order(
                spawn,
                "std::thread(",
                "apartment_type::multi_threaded",
                "body(*task)",
                "done.store(true)",
            )
        )

    def test_external_subfolders_and_all_package_formats_are_enumerated(self):
        scan = body(USB, "std::vector<std::filesystem::path> ListExternalGames()")
        self.assertIn("GetFilesAsync()", scan)
        self.assertIn("GetFoldersAsync()", scan)
        self.assertIn("pending.push_back(child)", scan)
        for extension in (".nsp", ".nsz", ".xci"):
            self.assertIn(f'L"{extension}"', scan)

    def test_ui_adopts_completed_snapshot_and_merges_both_sources_identically(self):
        tick = body(SHOP, "void TickShop(")
        adopt = body(tick, "if (storage_ && storage_->done.load())")
        self.assertTrue(
            in_order(
                adopt,
                "!storage_->cancel.load()",
                # An unchanged list must not rescan: ScanInstalled reads every library record.
                "storage_->result.external_files != external_installed_",
                "external_installed_ = std::move(storage_->result.external_files)",
                "ScanInstalled();",
            )
        )
        scan = body(SHOP, "void ScanInstalled()")
        add = body(scan, "const auto add =")
        for field in ("paths", "sizes", "mtimes", "files"):
            self.assertIn(f"installed_{field}_.push_back(", add)
        self.assertIn("installed_.push_back(Tinfoil::ParsePackage(", add)
        self.assertIn("add(ReadInstalledFile(it->path()));", scan)
        self.assertIn("for (const auto& file : external_installed_)\n            add(file);", scan)
        self.assertNotIn("last_write_time(", scan)
        # Rows and detail badges read this inventory on each draw, with no cached installed flag.
        self.assertIn("Installed(", body(SHOP, "void DrawDetail("))
        self.assertIn("installed_.begin()", body(SHOP, "void DrawDetail("))
        self.assertIn("installed_.size()", body(SHOP, "bool Installed("))

    def test_library_record_requires_matching_path_size_and_valid_mtime(self):
        scan = body(SHOP, "void ScanInstalled()")
        self.assertIn("LowerWide(path.lexically_normal().wstring())", scan)
        self.assertIn("LowerWide(installed_paths_[i].lexically_normal().wstring())", scan)
        self.assertIn("size.GetNumber() != static_cast<double>(installed_sizes_[i])", scan)
        self.assertIn("mtime.GetNumber() != static_cast<double>(installed_mtimes_[i])", scan)
        self.assertIn("installed_[i] = package", scan)
        metadata = body(SHOP, "InstalledFile ReadInstalledFile(")
        # The library's own helper, so a record's mtime and the Shop's can never drift apart.
        self.assertIn("Common::FS::GetSize(path)", metadata)
        self.assertIn("ModifiedSeconds(path)", metadata)
        header = (ROOT / "src/eden_uwp/ui/library.h").read_text()
        self.assertIn("std::int64_t ModifiedSeconds(const std::filesystem::path& file);", header)

    def test_download_completion_invalidates_old_snapshot_and_requests_fresh_merge(self):
        update = body(SHOP, "void UpdateDownload(")
        done = body(update, "if (outcome == Outcome::Done)")
        self.assertIn("ScanInstalled();", done)
        self.assertIn("storage_->cancel.store(true)", done)
        self.assertIn("storage_refresh_ = {};", done)
        tick = body(SHOP, "void TickShop(")
        self.assertTrue(
            in_order(
                tick,
                "UpdateDownload(now)",
                "storage_->done.load()",
                "if (!storage_ && now >= storage_refresh_)",
                "Spawn<StorageResult>",
            )
        )
        self.assertIn("std::chrono::seconds(5)", tick)


if __name__ == "__main__":
    unittest.main()
