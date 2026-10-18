// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend_common/library_transfer.h"
#include <cassert>
#include <map>
#include <string>

int main() {
  using namespace LibraryTransfer;
  assert(Target("internal/Updates/x.nsp", "internal", "hd") ==
         "hd/Updates/x.nsp");
  assert(Target("internal/x.nsp", "internal", "hd") == "hd/x.nsp");
  assert(Target("internal/DLC/nested/x.nsp", "internal", "hd") ==
         "hd/DLC/nested/x.nsp");
  assert(Target("internal/Base/x.nsp", "internal", "hd") !=
         Target("internal/Updates/x.nsp", "internal", "hd"));
  const std::vector<File> files{
      {"internal/Base/game.nsp", "hd/Base/game.nsp", 10},
      {"internal/Updates/update.nsp", "hd/Updates/update.nsp", 5}};
  for (int scenario = 0; scenario < 7; ++scenario) {
    std::map<fs::path, std::uint64_t> disk{{files[0].source, 10},
                                           {files[1].source, 5}};
    int copies = 0, removed = 0;
    if (scenario == 1)
      disk[files[1].target] = 99;
    IO io;
    io.exists = [&](const auto &path) { return disk.contains(path); };
    io.size = [&](const auto &path) { return disk[path]; };
    io.unchanged = [&](const auto &file) {
      return disk[file.source] == file.size && !(scenario == 5 && copies == 2);
    };
    io.copy = [&](const auto &file) {
      ++copies;
      if (scenario == 3 && copies == 2)
        return false;
      disk[file.target] = file.size - (scenario == 4 ? 1 : 0);
      return true;
    };
    io.remove = [&](const auto &path) {
      assert(copies == 2);
      assert(disk[files[0].target] == 10 && disk[files[1].target] == 5);
      ++removed;
      return disk.erase(path) == 1;
    };
    const std::optional<std::uint64_t> free =
        scenario == 6
            ? std::nullopt
            : std::optional<std::uint64_t>{scenario == 2 ? 0 : 1ull << 30};
    const auto result = Move(files, free, io);
    if (scenario == 0) {
      assert(result == Result::Success && removed == 2);
      assert(!disk.contains(files[0].source) &&
             !disk.contains(files[1].source));
    } else {
      assert(result != Result::Success && removed == 0);
      assert(disk[files[0].source] == 10 && disk[files[1].source] == 5);
      if (scenario == 1 || scenario == 2 || scenario == 6)
        assert(copies == 0);
    }
  }
}
