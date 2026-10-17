// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// NXBOX_JIT_DUALMAP_PROBE=1: before the game boots, checks whether this AppContainer can map one
// pagefile-backed section twice (a writable view and an executable view), run code through the
// executable view, and rewrite it through the writable view without any protection change. It
// logs one JIT_DUALMAP_PROBE line, compares the cost of an aliased write + instruction-cache flush
// with the VirtualProtectFromApp RW->RX round trip the JIT uses today, and never throws.
void NxboxJitDualMapProbe() noexcept;
