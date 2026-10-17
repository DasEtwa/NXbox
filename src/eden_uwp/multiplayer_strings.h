// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <iterator>
#include "eden_uwp/ui/strings.h"
namespace EdenXbox::Ui::Multiplayer {
enum class Text {
#define NXBOX_MULTIPLAYER_TEXT(id, value) id,
#include "eden_uwp/multiplayer_en.inc"
#undef NXBOX_MULTIPLAYER_TEXT
    Count
};
inline const wchar_t* Tr(Text text) {
#define NXBOX_MULTIPLAYER_TEXT(id, value) value,
    static constexpr const wchar_t* english[] = {
#include "eden_uwp/multiplayer_en.inc"
    };
    static constexpr const wchar_t* portuguese[] = {
#include "eden_uwp/multiplayer_pt.inc"
    };
#undef NXBOX_MULTIPLAYER_TEXT
    static_assert(std::size(english) == static_cast<std::size_t>(Text::Count));
    static_assert(std::size(portuguese) == std::size(english));
    return (CurrentLanguage() == Language::Portuguese ? portuguese
                                                      : english)[static_cast<std::size_t>(text)];
}
} // namespace EdenXbox::Ui::Multiplayer
