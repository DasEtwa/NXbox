// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/accounts_policy.h"
#include "core/hle/service/acc/nxbox_profile.h"
#include "eden_uwp/ui/avatar_composite.h"
#include <array>
#include <cassert>

int main() {
  using EdenXbox::Ui::AvatarBackgroundChannel;
  static_assert(AvatarBackgroundChannel(255, 0, 0) == 255);
  static_assert(AvatarBackgroundChannel(255, 0, 128) == 127);
  static_assert(AvatarBackgroundChannel(100, 50, 128) == 75);
  static_assert(AvatarBackgroundChannel(0, 255, 128) == 128);
  static_assert(AvatarBackgroundChannel(255, 0, 255) == 0);
  using EdenXbox::Ui::CompositeAvatarChannel;
  // Premultiplied fixture: straight (200, 100, 50, 128) over (20, 40, 80).
  static_assert(CompositeAvatarChannel(100, 128, 20) == 110);
  static_assert(CompositeAvatarChannel(50, 128, 40) == 70);
  static_assert(CompositeAvatarChannel(25, 128, 80) == 65);
  static_assert(CompositeAvatarChannel(0, 0, 255) == 255);
  static_assert(CompositeAvatarChannel(200, 255, 0) == 200);
  using namespace EdenXbox::Accounts;
  static_assert(!NeedsPicker(false, false, false));
  static_assert(!NeedsPicker(false, false, true));
  static_assert(NeedsPicker(false, true, true));
  static_assert(!NeedsPicker(true, false, true));
  static_assert(!NeedsPicker(true, true, false));
  static_assert(NeedsPicker(true, true, true));
  using namespace Service::Account::NXbox;
  const std::array<unsigned char, 4> backup{0xff, 0xd8, 0xff, 0xd9};
  const std::array<unsigned char, 4> custom{0xff, 0xd8, 0x00, 0xd9};
  assert(ShouldMigrate(1, "Player", backup, backup));
  assert(!ShouldMigrate(2, "Player", backup, backup));
  assert(!ShouldMigrate(1, "NXbox", backup, backup));
  assert(!ShouldMigrate(1, "Player 2", backup, backup));
  assert(!ShouldMigrate(1, "Player", custom, backup));
  assert(!ShouldMigrate(1, "Player", {}, backup));
  assert(!ShouldMigrate(1, "Player", std::span(backup).first(3), backup));
  assert(AvatarRelativePath("00112233-4455-6677-8899-aabbccddeeff") ==
         "system/save/8000000000000010/su/avators/"
         "00112233-4455-6677-8899-aabbccddeeff.jpg");
}
