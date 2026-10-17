# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later
"""Structural contracts for the UWP-only encrypted-shop handoff."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "src/eden_uwp/ui"
SHOP = (UI / "sources_screen.cpp").read_text()
LIBRARY = (UI / "library_screen.cpp").read_text()


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


def test_encrypted_reason_survives_mixed_sources_and_pages():
    fetch = body(SHOP, "FetchResult FetchSource(")
    assert re.search(
        r"if \(index.unsupported == Tinfoil::Unsupported::Encrypted\)\s*"
        r"result.encrypted_sources.insert\(request->url\)",
        fetch,
    )
    adopt = body(SHOP, "void AdoptFetch()")
    assert "encrypted_sources_.insert(fetch_->result.encrypted_sources.begin()," in adopt
    assert "fetch_->result.encrypted_sources.end())" in adopt
    assert "shop_ && !encrypted_sources_.empty()" in body(SHOP, "bool HasEncryptedShop()")
    assert "HasEncryptedShop() ? 1 : 0" in body(SHOP, "int BrowseRows()")
    assert "encrypted_sources_.clear()" in body(SHOP, "void LoadBrowse(")
    assert "encrypted_sources_.clear()" in body(SHOP, "void ReloadShop()")


def test_encrypted_shop_pill_and_install_hint():
    draw = body(SHOP, "void DrawEncryptedShop(")
    assert "tinfoil_available_ ? Text::ShopEncrypted : Text::ShopInstallTinfoil" in draw
    assert re.search(r"if \(tinfoil_available_\)\s*DrawChoicePill\(", draw)
    assert "Tr(Text::ShopOpenTinfoil)" in draw
    browse = body(SHOP, "void DrawBrowse(")
    assert "HasEncryptedShop() && static_cast<std::size_t>(i) == visible_.size()" in browse
    assert "DrawEncryptedShop(" in browse
    assert "!sidebar_focus_" in browse and "!navigation_focused_" in browse
    update = body(SHOP, "void UpdateBrowse(")
    assert re.search(
        r"if \(HasEncryptedShop\(\) && selected == visible_.size\(\)\)\s*"
        r"{\s*if \(tinfoil_available_\)\s*tinfoil_requested_ = true;",
        update,
    )


def test_tinfoil_lookup_uses_adopted_library_model():
    adopt = body(LIBRARY, "void AdoptScan()")
    assert adopt.index("games_ = scan_->Take()") < adopt.index('"050000BADDAD0000"')
    assert 'Game(i).title_id == "050000BADDAD0000"' in adopt
    assert "tinfoil_index_ = i;" in adopt
    assert "tinfoil_index_ = -1;" in body(LIBRARY, "void StartScan()")
    update = body(LIBRARY, "void Update(")
    assert "shop_->SetTinfoilAvailable(tinfoil_index_ >= 0)" in update
    assert "directory_iterator" not in update
    assert "LibraryScan" not in update
    assert "050000BADDAD0000" not in SHOP


def test_handoff_reuses_library_selection_and_game_txt_path():
    handle = body(LIBRARY, "void HandleInput(")
    assert re.search(
        r"if \(shop_->TakeTinfoilRequest\(\) && tinfoil_index_ >= 0 &&\s*"
        r"tinfoil_index_ < GameCount\(\)\)\s*{\s*selected_ = tinfoil_index_;\s*"
        r"Launch\(now\);\s*return;",
        handle,
    )
    assert "Launch(now);" in body(LIBRARY, "void Activate(")
    launch = body(LIBRARY, "void Launch(")
    assert "const GameEntry& game = Game(selected_);" in launch
    assert "result_ = winrt::to_string(game.path.wstring());" in launch
    assert "launched_ = selected_;" in launch
    assert "launching_ = true;" in launch
    assert "return launching_ ? result_ : std::string();" in body(LIBRARY, "std::string Run()")
    assert "std::exchange(tinfoil_requested_, false)" in body(SHOP, "bool TakeTinfoilRequest()")
    session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
    assert "chosen = Ui::RunLibrary(window, &choice);" in session
    assert "RememberChosenGame(local_state, chosen);" in session
    assert 'std::ofstream(local / "game.txt"' in body(session, "void RememberChosenGame(")
    # The shop can only signal a title launch, with no URL/credential payload.
    header = (UI / "sources_screen.h").read_text()
    assert "bool TakeTinfoilRequest();" in header
    assert "game.txt" not in SHOP
    assert "Clipboard" not in SHOP


def test_localized_tinfoil_guidance():
    expected = {
        "en": (
            "This shop is encrypted. Open it in Tinfoil.",
            "Open Tinfoil",
            "Install Tinfoil to use encrypted shops.",
        ),
        "pt_br": (
            "Esta loja é criptografada. Abra no Tinfoil.",
            "Abrir Tinfoil",
            "Instale o Tinfoil para usar lojas criptografadas.",
        ),
    }
    keys = ("ShopEncrypted", "ShopOpenTinfoil", "ShopInstallTinfoil")
    strings = (UI / "strings.h").read_text()
    for key in keys:
        assert key + "," in strings
    for language, messages in expected.items():
        translations = (UI / f"lang/{language}.inc").read_text()
        for key, message in zip(keys, messages, strict=True):
            assert f'{{Text::{key}, L"{message}"}}' in translations
