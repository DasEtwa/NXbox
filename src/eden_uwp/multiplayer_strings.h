// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <iterator>
#include "eden_uwp/ui/strings.h"
namespace EdenXbox::Ui::Multiplayer {
enum class Text {
    Title,
    Nickname,
    Address,
    Password,
    RoomName,
    Join,
    Create,
    Refresh,
    Leave,
    Empty,
    Working,
    Failed,
    Hosting,
    Players,
    Hints,
    Count
};
inline const wchar_t* Tr(Text text) {
    static constexpr const wchar_t* english[] = {
        L"Local multiplayer",
        L"Your nickname",
        L"Room address",
        L"Room password",
        L"Room name",
        L"Join by address",
        L"Create a room",
        L"Refresh public rooms",
        L"Leave room",
        L"Refresh to find a room, join by address, or create one for your friends.",
        L"Please wait…",
        L"Could not connect. Check the address, password and network, then retry.",
        L"Room created. Friends on your LAN can join your Xbox address. Internet hosting needs UDP "
        L"port 24872 forwarded.",
        L"Players",
        L"A  Select     B  Back     X  Refresh     Y  Leave room"};
    static constexpr const wchar_t* portuguese[] = {
        L"Multiplayer local",
        L"Seu apelido",
        L"Endereço da sala",
        L"Senha da sala",
        L"Nome da sala",
        L"Entrar por endereço",
        L"Criar uma sala",
        L"Atualizar salas públicas",
        L"Sair da sala",
        L"Atualize para encontrar uma sala, entre por endereço ou crie uma para seus amigos.",
        L"Aguarde…",
        L"Não foi possível conectar. Confira endereço, senha e rede e tente novamente.",
        L"Sala criada. Amigos na LAN podem entrar pelo endereço do Xbox. Pela internet, encaminhe "
        L"a porta UDP 24872.",
        L"Jogadores",
        L"A  Selecionar     B  Voltar     X  Atualizar     Y  Sair da sala"};
    static_assert(std::size(english) == static_cast<std::size_t>(Text::Count));
    static_assert(std::size(portuguese) == std::size(english));
    return (CurrentLanguage() == Language::Portuguese ? portuguese
                                                      : english)[static_cast<std::size_t>(text)];
}
} // namespace EdenXbox::Ui::Multiplayer
