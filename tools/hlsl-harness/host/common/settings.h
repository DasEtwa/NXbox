// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Standalone host policy: native resolution, no rescaling, verification
// enabled. This adapter is private to the harness; emulator settings are not
// modified.
namespace Settings {
struct BooleanSetting {
  bool value{};
  bool GetValue() const { return value; }
  operator bool() const { return value; }
};
struct Values {
  struct {
    unsigned up_scale{1};
    unsigned down_shift{};
    float up_factor{1.0f};
    float down_factor{1.0f};
    bool active{};
  } resolution_info;
  BooleanSetting rescale_hack;
  bool renderer_debug{true};
  bool disable_shader_loop_safety_checks{};
};
inline Values values;
} // namespace Settings
