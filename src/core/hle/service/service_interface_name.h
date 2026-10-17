// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace Service {

// Interface names such as IRequest and IStorage are reused across service namespaces.
// Compile-time type names disambiguate them without requiring RTTI in emulator builds.
template <typename T>
constexpr std::string_view ServiceInterfaceName() {
#ifdef _MSC_VER
    constexpr std::string_view signature{__FUNCSIG__};
    constexpr auto start =
        signature.find("ServiceInterfaceName<") + std::string_view{"ServiceInterfaceName<"}.size();
    auto name = signature.substr(start, signature.find(">(void)", start) - start);
    if (name.starts_with("class ")) {
        name.remove_prefix(6);
    } else if (name.starts_with("struct ")) {
        name.remove_prefix(7);
    }
    return name;
#else
    constexpr std::string_view signature{__PRETTY_FUNCTION__};
    constexpr auto start = signature.find("T = ") + 4;
    constexpr auto end = signature.find_first_of(";]", start);
    return signature.substr(start, end - start);
#endif
}

} // namespace Service
