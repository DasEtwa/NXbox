# NXbox Privacy Policy (English)

[Versão em português abaixo](#política-de-privacidade-do-nxbox-português)

**Last updated: 2026-10-18**

This policy covers **NXbox** (the Xbox app, including its Nextendo Network sign-in) and **NxbHelper**
(the PC tool in `tools/nxbox-pack`). It is written from the source code in this repository, so you
can check each statement there. Where something cannot be checked in this repository, the text says
so.

## The short version

- NXbox has no online accounts of its own, no analytics, no advertising and no tracking. It does not
  upload crash reports or logs.
- NXbox is free, open-source software made by **NSPX**. NSPX operates one server for it: a sign-in
  relay that is used only if you turn on Google Drive save sync (see below). Everything else NXbox
  contacts is run by someone else, or is a server you chose yourself.
- NXbox does not upload your games, keys or firmware to the internet. Your saves leave the console
  only if you turn on save sync.
- Nextendo Network, save sync, Sources and the shared shader cache are off until you set them up.

## Who runs NXbox

NXbox is published by NSPX under the GNU GPL (version 3 or later). It is an independent fork of the
Eden emulator and is not affiliated with Nintendo, Microsoft or the Eden team. Nextendo Network is a
separate community service run by its own operators, not by NSPX.

NSPX has no company servers that collect your data. The only server NSPX operates for NXbox is the
sign-in relay at `auth.nspx.dev` (Google Drive save sync only). Releases, the artwork index and the
source code are files on GitHub.

## What the Xbox app connects to

Each item says when it runs, where it connects and what it sends.

### Update check

- **When:** every time the game library opens. There is no setting to turn it off.
- **Connects to:** `api.github.com` (GitHub).
- **Sends:** a request for the latest NXbox release, with the User-Agent `NXbox-updater`. It does
  not send your version, a device ID or an account.
- If you choose to install an update, the package is downloaded from
  `github.com/NspxMiguel/NXbox/releases` and sent from the console to the Device Portal address you
  wrote in `portal.json` (normally the console itself).

### Artwork from the internet

- **When:** on by default. Settings, "Artwork from the internet", turns it off.
- **Connects to:** `raw.githubusercontent.com` (an index file in this repository) and
  `images.launchbox-app.com` (LaunchBox).
- **Sends:** the index is downloaded whole. Game names are matched against it on your console and
  are not sent. The image file names the app then requests do show LaunchBox which games' artwork
  your library asked for.

### Cheats

- **When:** you open the cheats screen for a game. The downloaded copy is reused for 24 hours.
- **Connects to:** `github.com/sthetix/nx-cheats-db` (GitHub releases).
- **Sends:** a request for the whole cheat database. No game name or title ID is in the request.

### Mods

- **When:** you open the mods screen for a game.
- **Connects to:** `gamebanana.com` (GameBanana API),
  `raw.githubusercontent.com/gamemoddesignbr/nx-links`, and the file addresses those listings give
  (for example GameBanana's file servers or GitHub releases).
- **Sends:** the game's name, to search GameBanana for it, then GameBanana's ID for that game. The
  nx-links index is downloaded whole and filtered on your console by title ID. The mod you pick is
  downloaded from the host that lists it.

### Sources

- **When:** only for servers you add yourself. NXbox ships none.
- **Connects to:** the address you typed.
- **Sends:** requests for that server's index, thumbnails, and the games you choose to download or
  stream, with the User-Agent `NXbox/1.0`, a `Referer`, and any extra headers that server's index
  asks for. If you typed `user:password@` in the address, it is sent as an HTTP Basic login to that
  server only. The server's operator sees your IP address and what you request.

### Multiplayer

- **When:** Settings, Multiplayer.
- **Connects to:** `api.ynet-fun.xyz` (public room list), and the room you join.
- **Sends:** opening the screen requests the public room list. Joining a room connects your console
  to that room's host, who sees your IP address, your nickname, and the name and ID of the game you
  run. A room you create is not published to the list.

### Save sync and Nextendo Network

Only after you sign in or turn them on. They have their own sections below.

### Shared shader cache

- **When:** off. Used only if you add `NXBOX_SHADER_SHARE_URL` to `nxbox_env.txt`.
- **Connects to:** the server at the address you wrote. None is built in.
- **Sends:** the per-game compiled shader cache is downloaded from, and uploaded to, that server.

Like any internet connection, every request reveals your IP address. NXbox adds no account ID,
device ID or advertising ID of its own.

**Games you run.** A game can use the console's internet connection like any program. NXbox blocks
name lookups for Nintendo's domains and a few other game-service domains by default. Whatever a game
sends is between you, the game and the server it talks to. NXbox does not collect or upload that
traffic.

## Save sync (optional)

Settings, SwitchSaveSync. It is off until you sign in. After that, saves sync for a game when it
opens and when it closes.

**Google Drive.**

1. NXbox asks the sign-in relay (`https://auth.nspx.dev/nxbox`) for a device code and shows it. You
   approve it on your phone at `google.com/device`. You sign in to Google there. NXbox never sees
   your Google password.
2. NXbox polls the relay until Google approves. The relay forwards the request to Google and returns
   Google's answer, which includes a refresh token. NXbox stores that token in the Windows
   Credential Locker (PasswordVault).
3. To get a short-lived access token, NXbox sends the refresh token to the relay, which forwards it
   to Google. The access token is then used only with Google's Drive API (`www.googleapis.com`).

NXbox asks for the `drive.file` permission, so it can only see files and folders it created itself,
not the rest of your Drive. For each synced game it uploads the save files of the active profile to
`Nintendo Switch Saves/<game name>/<profile name>/`, with their file names and modification times.
Console-wide saves use the folder name `console`. A Switch running SwitchSaveSync can read the same
folder.

**The sign-in relay.** It is run by NSPX and holds the Google OAuth client credential so that no
secret ships inside the app. By design of the requests above, it sees your IP address, the device
code, your refresh token and the access tokens in Google's replies while it forwards them. Its
source code is not in this repository, so this repository cannot prove what the server stores. NSPX
does not use it to build accounts, profiles or analytics. The domain is served through Cloudflare,
which handles the connection as any content network does.

**WebDAV.** Instead of Google, you can enter an `https://` WebDAV address, a user name and a
password. They are stored in the Credential Locker and the save files are sent to that server only.
Its operator sees them and your IP address.

**On the console.** Before a cloud save replaces the one on the console, NXbox keeps a backup in
`LocalState\savesync_backup`. Sync fingerprints are kept in `LocalState\savesync_state.json`.

## Nextendo Network (optional)

Settings, Online: Nextendo Network. It is off by default. NXbox ships with its public Nextendo
client ID and Nextendo's public server addresses; a `nextendo.json` file in `LocalState` can
override them. Nothing is sent to Nextendo until you sign in or turn online on.

**Sign-in.** NXbox opens `https://nextendo.network/api/oauth/authorize` in the Xbox browser using
OAuth with PKCE. The request carries NXbox's public client ID, the return address
`http://127.0.0.1:17822/callback`, a random state value, the PKCE challenge, and these scopes:
`identity friends presence game.matchmaking`. You sign in on Nextendo's website, so NXbox never sees
your Nextendo password. You then type the final browser address back into NXbox. NXbox exchanges the
one-time code (and the PKCE verifier) at `https://nextendo.network/api/oauth/token` for an access
token and a refresh token, then fetches a game token and your Nextendo account details
(`/api/nex-token`).

**What NXbox keeps.** In the Credential Locker (`NXbox.Nextendo.Account`): the refresh token, your
Nextendo ID, user name, friend code, and the ID of the local profile it is linked to. Other local
profiles cannot use it. The access token and the game token are kept in memory only. Friends and
presence data are kept in memory only and are not written to disk. NXbox does not log tokens, codes
or responses.

**While online is on.** NXbox contacts `nextendo.network` to:

- refresh the tokens (`/api/oauth/token` with the refresh token and client ID, `/api/nex-token`);
- fetch your friends list, with their names, IDs and presence, about every 45 seconds
  (`/api/friends`);
- send your presence (`/api/presence`): your online status, the title ID of the game you are
  running, and a small block of data (up to 192 bytes) that the game itself supplies;
- send a session heartbeat (`/api/nex-session`).

Each call carries your access token and NXbox's client ID, and Nextendo sees your IP address.
Nextendo decides what it shows to your friends and how long it keeps data. See its own terms.

**Game connections.** With online on, game connections to the hostnames that Nintendo-era online
services use (for example `*.nintendowifi.net`) are resolved to the Nextendo server addresses (the
defaults or those in `nextendo.json`), so what the game sends goes to Nextendo's servers. The
account token NXbox hands to the game contains your Nextendo game token, the game's version, random
identifiers created for that token and a fixed placeholder serial number. It is signed with a key
Nextendo gave you, or with a temporary key made on the console. NXbox does not read your console's
serial number, keys or a Nintendo account for this.

**Signing out.** Pressing X on the Nextendo screen while signed in deletes the stored credential and
the tokens in memory on the console. It does not contact Nextendo. To end NXbox's access or delete
your data on Nextendo's side, use Nextendo's own tools or ask its operators.

## NxbHelper

NxbHelper runs on your PC. It has no analytics and no account.

- **GitHub.** It asks `api.github.com` for the latest NXbox release (and, if there is none, for
  recent build artifacts) and downloads the runtime package. It checks the SHA-256 before using it.
  If you set `NXBHELPER_GITHUB_TOKEN`, that token is sent to `api.github.com` only. You can also give
  your own HTTPS address and checksum.
- **Your Xbox.** It talks to the Device Portal at the address you give, with your Device Portal
  login. To find the console, it opens a plain TCP connection to port 11443 on `xbox.local` and on
  the addresses of your local private networks. Nothing is sent in those probes.
- **Your login.** It stays in memory unless you choose to save it. If you do, it is stored in macOS
  Keychain, the Linux Secret Service, or the Windows Credential Locker. On Windows, if that fails,
  an encrypted file under `~/.local/share/nxbox-pack` is used instead.
- **Your files.** Games, keys and firmware go from your PC to your USB drive or to your own console.
  Profile pictures are converted by a temporary web page served only on `127.0.0.1`.

## What is stored on your device

- **`LocalState`**, the app's private folder: your library data, artwork and cheat caches, mods you
  installed, shader and code caches, local profiles (`accounts.json`), the list of Sources
  (`sources.json`, stored as you typed each address, so a password or token in an address is stored
  there in plain text), save sync backups, and the Eden data folder with your keys, firmware and
  saves.
- **Files you create:** `portal.json` (Device Portal host, user and password in plain text, only if
  you want in-app updates), `nextendo.json` (public Nextendo configuration), `nxbox_env.txt`
  (switches). Delete them when you no longer need them.
- **Credential Locker:** `NXbox.Nextendo.Account`, `NXbox.SaveSync` (Google refresh token or WebDAV
  login) and, if you provided one, `NXbox.Nextendo.SigningKey`.
- **Logs:** `eden_uwp_diag.txt` and `nxbox_crash.txt` in `LocalState`. They stay on the console and
  are never uploaded by NXbox. They can contain game names, title IDs, file paths and error details.
  Review them before you post them in a bug report. The code is written not to log passwords, tokens
  or full Source addresses.

## What NXbox never collects

Usage statistics, analytics, advertising IDs, device or console identifiers, your Xbox gamertag or
Microsoft account, your location, contacts, or a list of your games. (The artwork requests described
above do show LaunchBox which games' artwork your library asked for.) Eden's web service (telemetry,
public room announcements) and Discord Rich Presence are compiled out of the Xbox build. The Xbox
system itself may collect diagnostic data under Microsoft's own policy, which NXbox does not
control.

## Third parties

Each of these has its own privacy rules:

- GitHub:
  [privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)
  (releases, the artwork index, nx-cheats-db and nx-links are hosted there).
- Google (Drive and sign-in): [privacy policy](https://policies.google.com/privacy).
- LaunchBox (artwork):
  [privacy policy](https://www.launchbox-app.com/Resources/Documents/launchbox-privacy-policy.pdf).
- GameBanana (mods): [gamebanana.com](https://gamebanana.com).
- Nextendo Network: [nextendo.network](https://nextendo.network).
- Eden project (the public room list protocol): [eden-emu.dev](https://eden-emu.dev). The list host
  `api.ynet-fun.xyz` is not run by NSPX.
- Cloudflare: [privacy policy](https://www.cloudflare.com/privacypolicy/).
- Microsoft (Xbox): [privacy statement](https://privacy.microsoft.com/privacystatement).
- Discord (community server): [privacy policy](https://discord.com/privacy). The app does not
  connect to Discord.
- Servers you add (Sources, WebDAV, shader cache): their operators.

## Your choices and deleting your data

- **Uninstall NXbox** from the Xbox Dev Home or the Device Portal. Windows removes an app's
  `LocalState` when it is uninstalled. Sign out of Nextendo and save sync first, so the stored
  credentials are removed explicitly. NXbox cannot promise how the console handles the Credential
  Locker on uninstall.
- **Nextendo:** press X on the Nextendo screen, and turn online off.
- **Save sync:** Settings, SwitchSaveSync, A to sign out. This removes the Google token and the
  WebDAV login from the console. It does not revoke access at Google: do that at
  [myaccount.google.com/permissions](https://myaccount.google.com/permissions). Files already in
  your Drive or WebDAV server stay until you delete them there.
- **Artwork:** turn off "Artwork from the internet".
- **Other data:** Google, GitHub, Nextendo and the other third parties hold their own copies. Use
  their tools. NSPX keeps no user database to export or delete. If you are in Brazil (LGPD) or the
  EU or UK (GDPR), you can still write to the contact below.

## Children

NXbox is not directed to children and does not ask for your age. NSPX does not knowingly collect
personal data from children. Google and Nextendo have their own minimum ages. NXbox runs games you
supply, and community online servers are not moderated by NSPX. Parents should decide what a child
may play and where.

## Changes

If NXbox changes what it connects to or stores, this file is updated in the same change and the date
at the top changes. The Git history of `PRIVACY.md` shows every revision.

## Contact

Open an issue at <https://github.com/NspxMiguel/NXbox/issues>. Do not post passwords, tokens or
personal data in a public issue. Ask for a private channel and one will be arranged.

---

# Política de Privacidade do NXbox (Português)

[English version above](#nxbox-privacy-policy-english)

**Última atualização: 2026-10-18**

Esta política cobre o **NXbox** (o app do Xbox, incluindo o login da Nextendo Network) e o
**NxbHelper** (a ferramenta de PC em `tools/nxbox-pack`). Ela foi escrita a partir do código-fonte
deste repositório, então você pode conferir cada afirmação nele. Quando algo não puder ser conferido
neste repositório, o texto diz isso.

## Em resumo

- O NXbox não tem contas online próprias, nem análise de uso, nem publicidade, nem rastreamento. Ele
  não envia relatórios de falha nem logs.
- O NXbox é um software livre e de código aberto feito pela **NSPX**. A NSPX opera um servidor para
  ele: um relay de login usado apenas se você ligar o backup de saves no Google Drive (veja abaixo).
  Todo o resto com que o NXbox se conecta é operado por terceiros, ou é um servidor que você mesmo
  escolheu.
- O NXbox não envia seus jogos, chaves nem firmware para a internet. Seus saves só saem do console
  se você ligar a sincronização de saves.
- Nextendo Network, sincronização de saves, Sources e o cache de shaders compartilhado ficam
  desligados até você configurar.

## Quem opera o NXbox

O NXbox é publicado pela NSPX sob a GNU GPL (versão 3 ou posterior). É um fork independente do
emulador Eden e não é afiliado à Nintendo, à Microsoft nem à equipe do Eden. A Nextendo Network é um
serviço comunitário separado, operado pelos seus próprios responsáveis, não pela NSPX.

A NSPX não tem servidores de empresa que coletem seus dados. O único servidor que a NSPX opera para
o NXbox é o relay de login em `auth.nspx.dev` (somente para a sincronização com o Google Drive). Os
releases, o índice de imagens e o código-fonte são arquivos no GitHub.

## Com o que o app do Xbox se conecta

Cada item diz quando roda, com o que se conecta e o que envia.

### Verificação de atualização

- **Quando:** toda vez que a biblioteca de jogos abre. Não há opção para desligar.
- **Conecta com:** `api.github.com` (GitHub).
- **Envia:** um pedido da última versão do NXbox, com o User-Agent `NXbox-updater`. Não envia sua
  versão, um ID de aparelho nem uma conta.
- Se você escolher instalar uma atualização, o pacote é baixado de
  `github.com/NspxMiguel/NXbox/releases` e enviado do console para o endereço do Device Portal que
  você escreveu em `portal.json` (normalmente o próprio console).

### Imagens da internet

- **Quando:** ligado por padrão. Em Ajustes, "Artes da internet" desliga.
- **Conecta com:** `raw.githubusercontent.com` (um arquivo de índice deste repositório) e
  `images.launchbox-app.com` (LaunchBox).
- **Envia:** o índice é baixado inteiro. Os nomes dos jogos são comparados com ele no seu console e
  não são enviados. Os nomes dos arquivos de imagem que o app pede depois mostram, sim, ao LaunchBox
  de quais jogos a sua biblioteca pediu imagem.

### Cheats

- **Quando:** você abre a tela de cheats de um jogo. A cópia baixada é reaproveitada por 24 horas.
- **Conecta com:** `github.com/sthetix/nx-cheats-db` (releases do GitHub).
- **Envia:** um pedido do banco de cheats inteiro. Nenhum nome de jogo nem ID de título vai no
  pedido.

### Mods

- **Quando:** você abre a tela de mods de um jogo.
- **Conecta com:** `gamebanana.com` (API do GameBanana),
  `raw.githubusercontent.com/gamemoddesignbr/nx-links` e os endereços de arquivo que essas listas
  informam (por exemplo servidores de arquivos do GameBanana ou releases do GitHub).
- **Envia:** o nome do jogo, para buscá-lo no GameBanana, e depois o ID do jogo no GameBanana. O
  índice nx-links é baixado inteiro e filtrado no seu console pelo ID do título. O mod que você
  escolhe é baixado do host que o lista.

### Sources

- **Quando:** somente para servidores que você mesmo adiciona. O NXbox não traz nenhum.
- **Conecta com:** o endereço que você digitou.
- **Envia:** pedidos do índice do servidor, miniaturas e dos jogos que você escolhe baixar ou
  transmitir, com o User-Agent `NXbox/1.0`, um `Referer` e os cabeçalhos extras que o índice desse
  servidor pedir. Se você digitou `usuario:senha@` no endereço, isso vai como login HTTP Basic
  somente para esse servidor. O operador do servidor vê seu endereço IP e o que você pede.

### Multiplayer

- **Quando:** Ajustes, Multiplayer.
- **Conecta com:** `api.ynet-fun.xyz` (lista pública de salas) e a sala em que você entra.
- **Envia:** abrir a tela pede a lista pública de salas. Entrar numa sala conecta seu console ao
  host da sala, que vê seu endereço IP, seu apelido, e o nome e o ID do jogo que você roda. Uma sala
  que você cria não é publicada na lista.

### Sincronização de saves e Nextendo Network

Somente depois de você entrar na conta ou ligar. Elas têm seções próprias abaixo.

### Cache de shaders compartilhado

- **Quando:** desligado. Só é usado se você puser `NXBOX_SHADER_SHARE_URL` no `nxbox_env.txt`.
- **Conecta com:** o servidor no endereço que você escreveu. Nenhum vem embutido.
- **Envia:** o cache de shaders compilados de cada jogo é baixado desse servidor e enviado a ele.

Como em qualquer conexão com a internet, todo pedido revela seu endereço IP. O NXbox não acrescenta
ID de conta, ID de aparelho nem ID de publicidade próprios.

**Jogos que você roda.** Um jogo pode usar a conexão do console como qualquer programa. O NXbox
bloqueia por padrão a resolução de nomes dos domínios da Nintendo e de alguns outros domínios de
serviços de jogos. O que um jogo envia é entre você, o jogo e o servidor com que ele fala. O NXbox
não coleta nem envia esse tráfego.

## Sincronização de saves (opcional)

Ajustes, SwitchSaveSync. Fica desligada até você entrar na conta. Depois disso, os saves de um jogo
são sincronizados quando ele abre e quando fecha.

**Google Drive.**

1. O NXbox pede um código de dispositivo ao relay de login (`https://auth.nspx.dev/nxbox`) e o
   mostra. Você aprova no celular em `google.com/device`. É lá que você entra no Google. O NXbox
   nunca vê sua senha do Google.
2. O NXbox consulta o relay até o Google aprovar. O relay repassa o pedido ao Google e devolve a
   resposta dele, que inclui um refresh token. O NXbox guarda esse token no Credential Locker do
   Windows (PasswordVault).
3. Para obter um access token de curta duração, o NXbox envia o refresh token ao relay, que o
   repassa ao Google. O access token é usado somente com a API do Drive do Google
   (`www.googleapis.com`).

O NXbox pede a permissão `drive.file`, então só enxerga arquivos e pastas que ele mesmo criou, não o
resto do seu Drive. Para cada jogo sincronizado, ele envia os arquivos de save do perfil ativo para
`Nintendo Switch Saves/<nome do jogo>/<nome do perfil>/`, com nomes de arquivo e datas de
modificação. Saves do console inteiro usam a pasta `console`. Um Switch com o SwitchSaveSync
consegue ler a mesma pasta.

**O relay de login.** É operado pela NSPX e guarda a credencial OAuth do Google para que nenhum
segredo vá dentro do app. Pelo próprio desenho dos pedidos acima, ele vê seu endereço IP, o código
de dispositivo, seu refresh token e os access tokens nas respostas do Google enquanto os repassa. O
código-fonte dele não está neste repositório, então este repositório não consegue provar o que o
servidor guarda. A NSPX não o usa para montar contas, perfis nem estatísticas. O domínio é servido
pela Cloudflare, que cuida da conexão como qualquer rede de conteúdo.

**WebDAV.** No lugar do Google, você pode informar um endereço WebDAV `https://`, um usuário e uma
senha. Eles ficam no Credential Locker e os arquivos de save vão somente para esse servidor. O
operador dele os vê, e vê seu endereço IP.

**No console.** Antes de um save da nuvem substituir o do console, o NXbox guarda uma cópia em
`LocalState\savesync_backup`. As impressões digitais da sincronização ficam em
`LocalState\savesync_state.json`.

## Nextendo Network (opcional)

Ajustes, Online: Nextendo Network. Vem desligada. O NXbox já vem com o ID de cliente público dele e
os endereços públicos dos servidores da Nextendo; um arquivo `nextendo.json` em `LocalState` pode
substituí-los. Nada é enviado à Nextendo até você entrar na conta ou ligar o online.

**Login.** O NXbox abre `https://nextendo.network/api/oauth/authorize` no navegador do Xbox usando
OAuth com PKCE. O pedido leva o ID de cliente público do NXbox, o endereço de retorno
`http://127.0.0.1:17822/callback`, um valor de estado aleatório, o desafio PKCE e estes escopos:
`identity friends presence game.matchmaking`. Você entra no site da Nextendo, então o NXbox nunca vê
sua senha da Nextendo. Depois você digita de volta no NXbox o endereço final do navegador. O NXbox
troca o código de uso único (e o verificador PKCE) em `https://nextendo.network/api/oauth/token` por
um access token e um refresh token, e em seguida busca um token de jogo e os dados da sua conta
Nextendo (`/api/nex-token`).

**O que o NXbox guarda.** No Credential Locker (`NXbox.Nextendo.Account`): o refresh token, seu ID
na Nextendo, nome de usuário, código de amigo e o ID do perfil local ao qual a conta está ligada.
Outros perfis locais não podem usá-la. O access token e o token de jogo ficam só na memória. Os
dados de amigos e de presença ficam só na memória e não são gravados em disco. O NXbox não registra
tokens, códigos nem respostas em log.

**Com o online ligado.** O NXbox se conecta a `nextendo.network` para:

- renovar os tokens (`/api/oauth/token` com o refresh token e o ID de cliente, `/api/nex-token`);
- buscar sua lista de amigos, com nomes, IDs e presença, a cada cerca de 45 segundos
  (`/api/friends`);
- enviar sua presença (`/api/presence`): seu status online, o ID do título do jogo que você está
  rodando e um pequeno bloco de dados (até 192 bytes) que o próprio jogo fornece;
- enviar um sinal de sessão ativa (`/api/nex-session`).

Cada chamada leva seu access token e o ID de cliente do NXbox, e a Nextendo vê seu endereço IP. A
Nextendo decide o que mostra aos seus amigos e por quanto tempo guarda os dados. Veja os termos
dela.

**Conexões dos jogos.** Com o online ligado, as conexões dos jogos aos nomes de host que os serviços
online da era Nintendo usam (por exemplo `*.nintendowifi.net`) são resolvidas para os endereços de
servidor da Nextendo (os padrões ou os do `nextendo.json`), então o que o jogo envia vai para os
servidores da Nextendo. O token de conta que o NXbox entrega ao jogo contém seu token de jogo da
Nextendo, a versão do jogo, identificadores aleatórios criados para esse token e um número de série
fixo de preenchimento. Ele é assinado com uma chave que a Nextendo forneceu, ou com uma chave
temporária criada no console. O NXbox não lê o número de série do seu console, as chaves dele nem
uma conta Nintendo para isso.

**Sair da conta.** Apertar X na tela da Nextendo com a conta ligada apaga a credencial guardada e os
tokens da memória, no console. Isso não contacta a Nextendo. Para encerrar o acesso do NXbox ou
apagar seus dados do lado da Nextendo, use as ferramentas da própria Nextendo ou fale com os
responsáveis por ela.

## NxbHelper

O NxbHelper roda no seu PC. Ele não tem análise de uso nem conta.

- **GitHub.** Ele pergunta a `api.github.com` qual é a última versão do NXbox (e, se não houver,
  quais são os artefatos de build recentes) e baixa o pacote de runtime. Confere o SHA-256 antes de
  usar. Se você definir `NXBHELPER_GITHUB_TOKEN`, esse token é enviado somente a `api.github.com`.
  Você também pode informar o seu próprio endereço HTTPS e checksum.
- **Seu Xbox.** Ele fala com o Device Portal no endereço que você informar, com o seu login do
  Device Portal. Para achar o console, abre uma conexão TCP simples na porta 11443 de `xbox.local` e
  dos endereços das suas redes privadas locais. Nada é enviado nessas sondagens.
- **Seu login.** Fica só na memória, a menos que você escolha salvá-lo. Se salvar, vai para o
  Keychain do macOS, o Secret Service do Linux ou o Credential Locker do Windows. No Windows, se
  isso falhar, um arquivo criptografado em `~/.local/share/nxbox-pack` é usado no lugar.
- **Seus arquivos.** Jogos, chaves e firmware vão do seu PC para o seu pendrive ou para o seu
  próprio console. Fotos de perfil são convertidas por uma página temporária servida só em
  `127.0.0.1`.

## O que fica guardado no seu aparelho

- **`LocalState`**, a pasta privada do app: dados da biblioteca, caches de imagens e de cheats, mods
  instalados, caches de shaders e de código, perfis locais (`accounts.json`), a lista de Sources
  (`sources.json`, guardada como você digitou cada endereço, então uma senha ou token dentro de um
  endereço fica ali em texto puro), backups da sincronização de saves e a pasta de dados do Eden com
  suas chaves, firmware e saves.
- **Arquivos que você cria:** `portal.json` (host, usuário e senha do Device Portal em texto puro,
  só se você quiser atualizar pelo app), `nextendo.json` (configuração pública da Nextendo),
  `nxbox_env.txt` (chaves de configuração). Apague-os quando não precisar mais.
- **Credential Locker:** `NXbox.Nextendo.Account`, `NXbox.SaveSync` (refresh token do Google ou
  login WebDAV) e, se você forneceu uma, `NXbox.Nextendo.SigningKey`.
- **Logs:** `eden_uwp_diag.txt` e `nxbox_crash.txt` em `LocalState`. Ficam no console e o NXbox
  nunca os envia. Podem conter nomes de jogos, IDs de título, caminhos de arquivo e detalhes de
  erro. Revise-os antes de postar num relato de bug. O código foi escrito para não registrar senhas,
  tokens nem endereços completos de Sources.

## O que o NXbox nunca coleta

Estatísticas de uso, análise de uso, IDs de publicidade, identificadores do aparelho ou do console,
seu gamertag do Xbox ou conta Microsoft, sua localização, contatos, nem uma lista dos seus jogos.
(Os pedidos de imagens descritos acima mostram, sim, ao LaunchBox de quais jogos a sua biblioteca
pediu imagem.) O serviço web do Eden (telemetria, anúncio público de salas) e o Discord Rich
Presence são removidos na compilação do Xbox. O próprio sistema do Xbox pode coletar dados de
diagnóstico conforme a política da Microsoft, que o NXbox não controla.

## Terceiros

Cada um tem as suas próprias regras de privacidade:

- GitHub:
  [declaração de privacidade](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)
  (releases, o índice de imagens, o nx-cheats-db e o nx-links ficam hospedados lá).
- Google (Drive e login): [política de privacidade](https://policies.google.com/privacy).
- LaunchBox (imagens):
  [política de privacidade](https://www.launchbox-app.com/Resources/Documents/launchbox-privacy-policy.pdf).
- GameBanana (mods): [gamebanana.com](https://gamebanana.com).
- Nextendo Network: [nextendo.network](https://nextendo.network).
- Projeto Eden (o protocolo da lista pública de salas): [eden-emu.dev](https://eden-emu.dev). O host
  da lista, `api.ynet-fun.xyz`, não é operado pela NSPX.
- Cloudflare: [política de privacidade](https://www.cloudflare.com/privacypolicy/).
- Microsoft (Xbox): [declaração de privacidade](https://privacy.microsoft.com/privacystatement).
- Discord (servidor da comunidade): [política de privacidade](https://discord.com/privacy). O app
  não se conecta ao Discord.
- Servidores que você adiciona (Sources, WebDAV, cache de shaders): os operadores deles.

## Suas escolhas e como apagar seus dados

- **Desinstale o NXbox** pelo Dev Home do Xbox ou pelo Device Portal. O Windows remove o
  `LocalState` de um app quando ele é desinstalado. Saia da Nextendo e da sincronização de saves
  antes, para que as credenciais guardadas sejam removidas de forma explícita. O NXbox não pode
  prometer como o console trata o Credential Locker na desinstalação.
- **Nextendo:** aperte X na tela da Nextendo e desligue o online.
- **Sincronização de saves:** Ajustes, SwitchSaveSync, A para sair. Isso remove o token do Google e
  o login WebDAV do console. Não revoga o acesso no Google: faça isso em
  [myaccount.google.com/permissions](https://myaccount.google.com/permissions). Os arquivos que já
  estão no seu Drive ou servidor WebDAV ficam até você apagá-los lá.
- **Imagens:** desligue "Artes da internet".
- **Outros dados:** Google, GitHub, Nextendo e os outros terceiros guardam as suas próprias cópias.
  Use as ferramentas deles. A NSPX não mantém base de usuários para exportar ou apagar. Se você está
  no Brasil (LGPD) ou na UE ou Reino Unido (GDPR), ainda pode escrever para o contato abaixo.

## Crianças

O NXbox não é direcionado a crianças e não pergunta sua idade. A NSPX não coleta conscientemente
dados pessoais de crianças. O Google e a Nextendo têm as suas próprias idades mínimas. O NXbox roda
jogos que você fornece, e os servidores online da comunidade não são moderados pela NSPX. Cabe aos
responsáveis decidir o que uma criança pode jogar e onde.

## Mudanças

Se o NXbox mudar o que acessa ou guarda, este arquivo é atualizado na mesma alteração e a data no
topo muda. O histórico do Git de `PRIVACY.md` mostra cada revisão.

## Contato

Abra uma issue em <https://github.com/NspxMiguel/NXbox/issues>. Não publique senhas, tokens nem
dados pessoais numa issue pública. Peça um canal privado e um será combinado.
