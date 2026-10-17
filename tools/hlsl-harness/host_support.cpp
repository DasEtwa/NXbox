// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/logging.h"
#include <iostream>
#include <stdexcept>

void AssertFailSoftImpl() {
  throw std::runtime_error("shader recompiler assertion failed");
}
[[noreturn]] void AssertFatalImpl() {
  throw std::runtime_error("shader recompiler reached unreachable code");
}
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char *filename, unsigned int line,
                       const char *, fmt::string_view format,
                       const fmt::format_args &args) {
  std::cerr << filename << ':' << line << ": " << fmt::vformat(format, args)
            << '\n';
}
} // namespace Common::Log
