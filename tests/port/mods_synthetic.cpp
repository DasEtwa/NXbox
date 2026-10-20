// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled by test_60fps_mods.py with the production algorithms inserted below.
#include "core/memory/dmnt_cheat_types.h"
#include "eden_uwp/ui/mods_paths.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
std::vector<std::string> diagnostic_lines;
#define LOG_INFO(category, ...)                                                \
  diagnostic_lines.push_back(fmt::format(__VA_ARGS__))
#define LOG_WARNING(category, ...)                                             \
  diagnostic_lines.push_back(fmt::format(__VA_ARGS__))
#define LOG_ERROR(category, ...)                                               \
  diagnostic_lines.push_back(fmt::format(__VA_ARGS__))
namespace Common {
template <class K, class V> using unordered_map = std::unordered_map<K, V>;
u16 swap16(u16 n) { return (n << 8) | (n >> 8); }
std::string ToLower(std::string s) {
  for (auto &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
template <typename C>
std::string HexToString(const C &bytes, bool upper = true) {
  std::string s;
  for (u8 b : bytes)
    s += fmt::format(fmt::runtime(upper ? "{:02X}" : "{:02x}"), b);
  return s;
}
std::vector<u8> HexStringToVector(std::string_view s, bool little_endian) {
  std::vector<u8> out;
  for (size_t i = 0; i < s.size(); i += 2)
    out.push_back(
        static_cast<u8>(std::stoul(std::string{s.substr(i, 2)}, nullptr, 16)));
  if (little_endian)
    std::reverse(out.begin(), out.end());
  return out;
}
template <size_t N> std::array<u8, N> HexStringToArray(std::string_view s) {
  const auto bytes = HexStringToVector(s, false);
  std::array<u8, N> out{};
  std::copy_n(bytes.begin(), N, out.begin());
  return out;
}
constexpr u32 MakeMagic(char a, char b, char c, char d) {
  return u32(a) | (u32(b) << 8) | (u32(c) << 16) | (u32(d) << 24);
}
} // namespace Common
namespace Loader {
struct NSOHeader {
  u32 magic;
  std::array<u8, 0x3c> before_id;
  std::array<u8, 0x20> build_id;
  std::array<u8, 0xa0> after_id;
};
static_assert(sizeof(NSOHeader) == 0x100);
} // namespace Loader
namespace Settings {
struct {
  bool dump_nso = false;
} values;
} // namespace Settings
namespace Core::Memory {
struct TextCheatParser {
  std::vector<CheatEntry> Parse(std::string_view) const;
};
} // namespace Core::Memory
// PRODUCTION_CHEATS
namespace FileSys {
struct Directory;
struct VectorVfsFile;
using VirtualDir = std::shared_ptr<Directory>;
using VirtualFile = std::shared_ptr<VectorVfsFile>;
struct VectorVfsFile {
  std::vector<u8> data;
  std::string name;
  VirtualDir parent;
  VectorVfsFile(std::vector<u8> bytes, std::string label = {},
                VirtualDir dir = {})
      : data(std::move(bytes)), name(std::move(label)), parent(std::move(dir)) {
  }
  std::string GetName() const { return name; }
  std::string GetExtension() const {
    return name.substr(name.find_last_of('.') + 1);
  }
  std::string GetFullPath() const { return name; }
  VirtualDir GetContainingDirectory() const { return parent; }
  size_t GetSize() const { return data.size(); }
  std::vector<u8> ReadAllBytes() const { return data; }
  std::vector<u8> ReadBytes(size_t n) const {
    return {data.begin(), data.begin() + std::min(n, data.size())};
  }
  size_t Read(u8 *out, size_t n, size_t offset = 0) const {
    if (offset >= data.size())
      return 0;
    n = std::min(n, data.size() - offset);
    std::memcpy(out, data.data() + offset, n);
    return n;
  }
  template <class T> size_t ReadObject(T *out, size_t offset) const {
    return Read(reinterpret_cast<u8 *>(out), sizeof(T), offset);
  }
  std::optional<u8> ReadByte(size_t offset) const {
    return offset < data.size() ? std::optional<u8>{data[offset]}
                                : std::nullopt;
  }
  void Resize(size_t n) { data.resize(n); }
  void WriteBytes(const std::vector<u8> &bytes) { data = bytes; }
};
struct Directory {
  std::string name;
  std::vector<VirtualFile> files;
  std::vector<VirtualDir> dirs;
  std::string GetName() const { return name; }
  auto GetFiles() const { return files; }
  auto GetSubdirectories() const { return dirs; }
  VirtualFile CreateFile(const std::string &) const { return {}; }
};
struct LayeredVfsDirectory {
  static VirtualDir MakeLayeredDirectory(std::vector<VirtualDir> roots) {
    if (roots.empty())
      return {};
    if (roots.size() == 1)
      return roots.front();
    auto result = std::make_shared<Directory>();
    for (const auto &root : roots) {
      result->files.insert(result->files.end(), root->files.begin(),
                           root->files.end());
      result->dirs.insert(result->dirs.end(), root->dirs.begin(),
                          root->dirs.end());
    }
    return result;
  }
};
struct Controller {
  VirtualDir root;
  VirtualDir GetModificationDumpRoot(u64) const { return {}; }
  VirtualDir GetModificationLoadRoot(u64) const { return root; }
  VirtualDir GetSDMCModificationLoadRoot(u64) const { return {}; }
};
VirtualDir GetOrCreateDirectoryRelative(VirtualDir, const char *) { return {}; }
} // namespace FileSys
// PRODUCTION_DECLARATIONS
// PRODUCTION_IPS
namespace FileSys {
std::mutex additional_mod_roots_mutex;
std::vector<VirtualDir> additional_mod_roots;
// PRODUCTION_HELPERS
struct PatchManager {
  using BuildID = std::array<u8, 0x20>;
  u64 title_id = 0x0100ABCDEF789000;
  Controller fs_controller;
  std::optional<u64> parent_title_id;
  std::vector<std::string> disabled;
  VirtualDir GetModificationLoadRoot(bool sdmc = false) const;
  static void SetAdditionalModLoadRoots(std::vector<VirtualDir>);
  std::vector<Core::Memory::CheatEntry> CreateCheatList(const BuildID &) const;
  auto GetDisabledAddons() const { return disabled; }
  std::vector<VirtualFile> CollectPatches(const std::vector<VirtualDir> &,
                                          const std::string &) const;
  std::vector<u8> PatchNSO(const std::vector<u8> &, const std::string &) const;
  bool HasNSOPatch(const BuildID &, std::string_view) const;
};
// PRODUCTION_METHODS
} // namespace FileSys

int main() {
  using namespace FileSys;
  Loader::NSOHeader header{};
  header.magic = Common::MakeMagic('N', 'S', 'O', '0');
  header.build_id[0] = 0xab;
  header.build_id[1] = 0xcd;
  std::vector<u8> nso(0x180, 0);
  std::memcpy(nso.data(), &header, sizeof(header));
  const auto original = nso;
  auto exefs = std::make_shared<Directory>(Directory{"ExEfS", {}, {}});
  auto mod = std::make_shared<Directory>(Directory{"60 FPS", {}, {exefs}});
  auto root = std::make_shared<Directory>(Directory{"title", {}, {mod}});
  PatchManager manager;
  manager.fs_controller.root = root;
  // IPS32 uses a four-byte big-endian offset, independent of the extension's
  // case.
  exefs->files.push_back(std::make_shared<VectorVfsFile>(
      std::vector<u8>{'I', 'P', 'S', '3', '2', 0, 0, 1, 0x10, 0, 2, 0x60, 0xab,
                      'E', 'E', 'O', 'F'},
      "abcd.IPS32", exefs));
  assert(manager.HasNSOPatch(header.build_id, "main"));
  auto patched = manager.PatchNSO(nso, "main");
  assert(patched[0x110] == 0x60 && patched[0x111] == 0xab);
  assert(
      std::equal(original.begin(), original.begin() + 0x100, patched.begin()));
  // A text patch gets its ID from @nsobid, never its arbitrary filename.
  const std::string text =
      "@nsobid ABCD\n@flag offset_shift 0x100\n@enabled\n00000020 "
      "11223344\n@disabled\n00000030 FFFFFFFF\n@stop\n";
  exefs->files.push_back(std::make_shared<VectorVfsFile>(
      std::vector<u8>{text.begin(), text.end()}, "unlock.PCHTXT", exefs));
  patched = manager.PatchNSO(nso, "main");
  assert(patched[0x110] == 0x60);
  assert(patched[0x120] == 0x44 && patched[0x123] == 0x11);
  assert(patched[0x130] == 0);
  manager.disabled = {"60 FPS"};
  assert(!manager.HasNSOPatch(header.build_id, "main"));
  assert(manager.PatchNSO(nso, "main") == original);
  manager.disabled.clear();
  auto wrong_id = header.build_id;
  wrong_id[0] = 0xee;
  assert(!manager.HasNSOPatch(wrong_id, "main"));
  setenv("NXBOX_MODS", "0", 1);
  assert(!manager.HasNSOPatch(header.build_id, "main"));
  assert(manager.PatchNSO(nso, "main") == original);
  unsetenv("NXBOX_MODS");
  assert(manager.PatchNSO(std::vector<u8>{1, 2, 3}, "main") ==
         std::vector<u8>({1, 2, 3}));
  // Standard IPS remains supported.
  exefs->files = {std::make_shared<VectorVfsFile>(
      std::vector<u8>{'P', 'A', 'T', 'C', 'H', 0, 1, 0x40, 0, 1, 0x42, 'E', 'O',
                      'F'},
      "ABCD.ips", exefs)};
  assert(manager.PatchNSO(nso, "main")[0x140] == 0x42);
  // Failed IPS32 must leave the image untouched.
  exefs->files = {std::make_shared<VectorVfsFile>(
      std::vector<u8>{'b', 'a', 'd'}, "ABCD.ips32", exefs)};
  assert(manager.PatchNSO(nso, "main") == original);
  const auto paths = EdenXbox::Ui::ModLoadPaths(
      "/LocalState", {"/USB/NXbox/games", "/USB/NXbox/games", "/USB/MyGame"});
  assert(paths.size() == 3);
  assert(paths[0] == "/LocalState/load");
  assert(paths[1] == "/USB/NXbox/games/load");
  assert(paths[2] == "/USB/MyGame/load");
  // Discover a mixed-case title directory solely through an additional load
  // root.
  manager.fs_controller.root = nullptr;
  root->name = "0100aBcDeF789000";
  auto load = std::make_shared<Directory>(Directory{"load", {}, {root}});
  PatchManager::SetAdditionalModLoadRoots({nullptr, load});
  assert(manager.GetModificationLoadRoot() == root);
  assert(manager.HasNSOPatch(header.build_id, "main"));
  // The actual Atmosphere parser and cheat-list reader must load each file
  // once.
  const std::string cheat_text = "[60 FPS]\n04000000 00123456 00000001\n";
  auto cheats = std::make_shared<Directory>(Directory{"cheats", {}, {}});
  cheats->files.push_back(std::make_shared<VectorVfsFile>(
      std::vector<u8>{cheat_text.begin(), cheat_text.end()},
      "aBcD000000000000.TxT", cheats));
  mod->dirs.push_back(cheats);
  auto cheat_list = manager.CreateCheatList(header.build_id);
  assert(std::count_if(cheat_list.begin(), cheat_list.end(),
                       [](const auto &entry) { return entry.enabled; }) == 1);
  manager.disabled = {"60 FPS"};
  assert(manager.CreateCheatList(header.build_id).empty());
  manager.disabled.clear();
  setenv("NXBOX_MODS", "0", 1);
  assert(manager.CreateCheatList(header.build_id).empty());
  assert(!manager.GetModificationLoadRoot());
  unsetenv("NXBOX_MODS");
  const auto contains_log = [](std::string_view marker) {
    return std::any_of(diagnostic_lines.begin(), diagnostic_lines.end(),
                       [&](const auto &line) {
                         return line.find(marker) != std::string::npos;
                       });
  };
  assert(contains_log("NXBOX_MOD_APPLIED"));
  assert(contains_log("NXBOX_MOD_FAILED"));
  assert(contains_log("NXBOX_CHEAT_LOADED"));
  PatchManager::SetAdditionalModLoadRoots({});
  assert(!manager.GetModificationLoadRoot());
  std::cout << "synthetic NSO mods: PASS (IPS, IPS32, pchtxt, IDs, disabled, "
               "opt-out, header, paths, cheats, logs)\n";
}
