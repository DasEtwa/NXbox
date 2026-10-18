# Guia do NXbox (Português)

[English version below](#nxbox-guide-english)

Este guia leva você do zero até o primeiro jogo rodando. Ele é para quem nunca usou o Device Portal. Siga os passos **na ordem**. Não pule nenhum.

## Antes de começar

Você precisa de:

- Um Xbox Series X ou S em **modo desenvolvedor** (Dev Mode).
- Um PC ou celular na mesma rede, para abrir o Device Portal.
- Um pendrive ou HD formatado em **exFAT ou NTFS**.
- Os **seus** dumps, tirados do **seu** Switch: jogos (`.nsp`, `.nsz`, `.xci`, `.xcz`), `prod.keys` e `title.keys`, e o firmware (arquivos `.nca`).

O NXbox não vem com nada disso. Ninguém compartilha esses arquivos aqui.

Três regras para não quebrar nada:

1. Faça os passos na ordem.
2. No Passo 4, o Xbox pergunta o que fazer com o pendrive. **Nunca escolha "games & apps"**: isso apaga tudo.
3. Se algo sair diferente do que o guia diz que você vai ver, pare e vá para **Deu problema?** no fim.

## Passo 1: prepare o pendrive no PC

Há dois caminhos. Os dois terminam com o pendrive no mesmo formato. O caminho fácil é o recomendado.

### Caminho fácil: NxHelper (recomendado)

O NxHelper (antes chamado NXbox Pack) é um programa para o seu computador. Baixe o arquivo do seu sistema na [página do último release](https://github.com/NspxMiguel/NXbox/releases/latest):

- Windows: `NxHelper-windows-x64.zip`
- macOS (chip Apple, arm64): `NxHelper-macos-arm64.zip`
- Linux: `NxHelper-linux-x64.zip`

Ele faz quatro coisas:

- **Prepara o pendrive para você.** É o uso recomendado: as pastas e os arquivos ficam no lugar certo, sem você montar nada à mão.
- **Confere um pendrive já preparado** e diz o que está errado: as keys, a versão do firmware em relação à das keys, downloads incompletos e pastas no lugar errado.
- **Envia jogos** para o NXbox já instalado no Xbox, pelo Device Portal. Isso só vale depois do Passo 2.
- **Descompacta firmware.** Ele pega um `.zip` de firmware ou um `rawnand.bin` e coloca o conteúdo na pasta certa.

Como saber que deu certo: depois de preparar o pendrive, rode a conferência. Se ela não apontar nenhum problema, o pendrive está pronto. Siga para o Passo 2.

### Caminho manual (alternativa)

Monte o pendrive no PC exatamente assim:

```
(raiz do pendrive)
├─ prod.keys       ← aqui, ou dentro de uma pasta "switch"
├─ title.keys
└─ NXbox
   ├─ games        ← jogos, updates e DLC, tudo junto
   └─ firmware     ← os .nca do firmware
```

- **Keys:** o NXbox procura `prod.keys` e `title.keys` primeiro na pasta `switch` do pendrive e depois na raiz.
- **Firmware:** o NXbox lê a pasta `NXbox\firmware` ou a pasta `firmware` na raiz do pendrive. Os `.nca` podem estar soltos ou dentro de **uma** subpasta, por exemplo `firmware\Firmware 21.2.0\*.nca`.
- **Nada de `.zip`.** O NXbox não lê firmware compactado. Descompacte antes (o NxHelper faz isso).
- **As keys precisam ser de um firmware igual ou mais novo** que o firmware que você copiou.
- Confira que **todos os arquivos terminaram de copiar ou baixar**. Um arquivo incompleto causa erro.

Como saber que deu certo: ao abrir o pendrive no PC, você vê essa estrutura, e a pasta do firmware está cheia de arquivos `.nca`.

## Passo 2: instale o NXbox

1. No Xbox, abra o **Dev Home** e ligue o **Device Portal** (Remote Access). Anote o endereço que aparece, tipo `https://192.168.x.x:11443`.
2. Abra esse endereço no navegador do PC e entre com o usuário e a senha que você criou. O navegador pode avisar que a conexão não é segura. Isso é normal no Device Portal: continue. Deu certo quando a página do Device Portal abre.
3. Em **Settings**, ligue **DefaultUWPContentTypeToGame**. Faça isso **antes** de instalar. Deu certo quando a opção aparece ligada.
4. Baixe o `NXbox_game_x.x.x.x_x64.appx` do [último release](https://github.com/NspxMiguel/NXbox/releases/latest).
5. Na **Home** do Device Portal: **Add** → escolha o `.appx` → **Next** → **Start**. Deu certo quando o NXbox aparece no Dev Home do Xbox.

## Passo 3: confira se é Game

No Dev Home: NXbox → **View details** → **App type**. Tem que estar **Game**.

Como **App**, o NXbox ganha só 1 GB de memória e os jogos fecham com `bad allocation`. Se estiver como **App**, troque para **Game** ali mesmo. Se não mudar, refaça o Passo 2.3 e instale de novo.

## Passo 4: plugue o pendrive no Xbox

O Xbox pergunta o que fazer com o pendrive. Escolha **Use for media**.

**Nunca escolha "games & apps": isso formata o drive e apaga tudo.**

Deu certo quando o Xbox deixa de perguntar e os arquivos continuam no pendrive.

## Passo 5: abra o NXbox

1. Na tela **Pendrive detectado**, escolha **Usar como SSD externo**. Os jogos ficam no pendrive e as keys vão sozinhas para o lugar certo.
2. Na primeira abertura, o NXbox pega as keys do pendrive e instala o firmware. **Você não precisa fechar e abrir de novo.** Deu certo quando a biblioteca mostra **Firmware instalado** e a versão. A pasta do firmware no pendrive muda de nome para `firmware_installed`. Isso é normal.
3. Escolha o jogo e aperte A. Na primeira vez, um `.nsz` demora alguns minutos convertendo. Deu certo quando o jogo abre.

## Deu problema? Confira nesta ordem

Se você usou o NxHelper, rode a conferência dele no pendrive primeiro. Ela aponta keys, versão do firmware, downloads incompletos e pastas erradas.

- **O jogo fecha na hora ou aparece `bad allocation`:** o NXbox está como App. Volte no Passo 3.
- **Ele pede firmware, mas o firmware está na pasta:** os `.nca` precisam estar em `NXbox\firmware` ou em `firmware` na raiz, soltos ou dentro de uma única subpasta. Sem `.zip`. Depois de instalado, a pasta muda de nome para `firmware_installed`. Se o NXbox pedir o firmware de novo (por exemplo, depois de desinstalar e instalar outra vez), volte o nome para `firmware`.
- **Erro de instalação do firmware:** o seu `prod.keys` é mais velho que esse firmware. Tire as keys de novo no Switch atualizado.
- **Tela preta pra sempre:** confira se o firmware está instalado (Passo 5.2). Se estiver, mande os logs.
- **O NXbox não acha o pendrive:** ele tem que estar em exFAT ou NTFS, e você tem que ter escolhido **Use for media** no Passo 4.

### Como mandar os logs

Device Portal → **File explorer** → **LocalAppData** → a pasta que começa com `NSPX.NXbox` → **LocalState**. Baixe `nxbox_crash.txt` e `eden_uwp_diag.txt` e poste no canal **#bugs** do [Discord](https://discord.gg/2SxEDzVyJM), dizendo o jogo, o console (X ou S) e o que aconteceu.

Rodou ou não rodou? Conte no canal **#compatibilidade** do Discord: um post por jogo, com o fps. O Series S roda, mas o autor não tem um para testar, então os relatos de vocês valem muito.

---

# NXbox guide (English)

[Versão em português acima](#guia-do-nxbox-português)

This guide takes you from zero to your first game running. It is written for people who have never used the Device Portal. Follow the steps **in order**. Do not skip any.

## Before you start

You need:

- An Xbox Series X or S in **Developer Mode** (Dev Mode).
- A PC or phone on the same network, to open the Device Portal.
- A USB stick or drive formatted as **exFAT or NTFS**.
- **Your own** dumps from **your own** Switch: games (`.nsp`, `.nsz`, `.xci`, `.xcz`), `prod.keys` and `title.keys`, and the firmware (`.nca` files).

NXbox ships none of these. Nobody shares these files here.

Three rules so you do not break anything:

1. Do the steps in order.
2. In Step 4, the Xbox asks what to do with the drive. **Never pick "games & apps"**: it erases everything.
3. If what you see differs from what the guide says you will see, stop and go to **Something wrong?** at the end.

## Step 1: set up the USB drive on your PC

There are two ways. Both end with the drive in the same layout. The easy way is the recommended one.

### Easy way: NxHelper (recommended)

NxHelper (formerly called NXbox Pack) is a program for your computer. Download the file for your system from the [latest release page](https://github.com/NspxMiguel/NXbox/releases/latest):

- Windows: `NxHelper-windows-x64.zip`
- macOS (Apple chip, arm64): `NxHelper-macos-arm64.zip`
- Linux: `NxHelper-linux-x64.zip`

It does four things:

- **Prepares the drive for you.** This is the recommended use: the folders and files end up in the right places, and you assemble nothing by hand.
- **Checks a drive that is already prepared** and tells you what is wrong: the keys, the firmware version compared with the keys, incomplete downloads, and folders in the wrong place.
- **Sends games** to the NXbox already installed on the Xbox, over the Device Portal. This only works after Step 2.
- **Unpacks firmware.** It takes a firmware `.zip` or a `rawnand.bin` and puts the contents in the right folder.

How to tell it worked: after preparing the drive, run the check. If it reports no problem, the drive is ready. Go to Step 2.

### Manual way (alternative)

Set up the drive on your PC exactly like this:

```
(drive root)
├─ prod.keys       ← here, or inside a folder called "switch"
├─ title.keys
└─ NXbox
   ├─ games        ← games, updates and DLC, all together
   └─ firmware     ← the firmware .nca files
```

- **Keys:** NXbox looks for `prod.keys` and `title.keys` in the drive's `switch` folder first, then in the drive root.
- **Firmware:** NXbox reads the `NXbox\firmware` folder, or the `firmware` folder at the drive root. The `.nca` files can be loose or inside **one** subfolder, for example `firmware\Firmware 21.2.0\*.nca`.
- **No `.zip`.** NXbox does not read a compressed firmware. Unpack it first (NxHelper can do this).
- **The keys must come from a firmware the same or newer** than the firmware you copied.
- Make sure **every file has finished copying or downloading**. An incomplete file causes an error.

How to tell it worked: when you open the drive on your PC, you see this layout, and the firmware folder is full of `.nca` files.

## Step 2: install NXbox

1. On the Xbox, open **Dev Home** and turn on the **Device Portal** (Remote Access). Note the address it shows, like `https://192.168.x.x:11443`.
2. Open that address in your PC's browser and sign in with the username and password you created. The browser may warn that the connection is not secure. This is normal for the Device Portal: continue. It worked when the Device Portal page opens.
3. Under **Settings**, turn on **DefaultUWPContentTypeToGame**. Do this **before** installing. It worked when the option shows as on.
4. Download `NXbox_game_x.x.x.x_x64.appx` from the [latest release](https://github.com/NspxMiguel/NXbox/releases/latest).
5. On the Device Portal **Home**: **Add** → pick the `.appx` → **Next** → **Start**. It worked when NXbox shows up in the Xbox's Dev Home.

## Step 3: check that it is a Game

In Dev Home: NXbox → **View details** → **App type**. It must say **Game**.

As an **App**, NXbox gets only 1 GB of memory and games close with `bad allocation`. If it says **App**, change it to **Game** right there. If it will not change, redo Step 2.3 and install again.

## Step 4: plug the USB drive into the Xbox

The Xbox asks what to do with the drive. Pick **Use for media**.

**Never pick "games & apps": that formats the drive and erases everything.**

It worked when the Xbox stops asking and your files are still on the drive.

## Step 5: open NXbox

1. On the **USB drive detected** screen, pick **Use as an external SSD**. The games stay on the drive and the keys go to the right place on their own.
2. On the first launch, NXbox takes the keys from the drive and installs the firmware. **You do not need to close and reopen it.** It worked when the library shows **Installed firmware** and its version. The firmware folder on the drive is renamed to `firmware_installed`. This is normal.
3. Pick a game and press A. The first time, an `.nsz` takes a few minutes to convert. It worked when the game starts.

## Something wrong? Check in this order

If you used NxHelper, run its check on the drive first. It points out keys, firmware version, incomplete downloads and wrong folders.

- **The game closes right away, or `bad allocation` appears:** NXbox is set as an App. Go back to Step 3.
- **It asks for firmware, but the firmware is in the folder:** the `.nca` files must be in `NXbox\firmware` or in `firmware` at the drive root, loose or inside a single subfolder. No `.zip`. Once installed, the folder is renamed to `firmware_installed`. If NXbox asks for the firmware again (for example after you uninstalled and reinstalled it), rename the folder back to `firmware`.
- **Firmware install error:** your `prod.keys` is older than that firmware. Dump the keys again from the updated Switch.
- **Black screen forever:** check that the firmware is installed (Step 5.2). If it is, send the logs.
- **NXbox doesn't find the USB drive:** it must be exFAT or NTFS, and you must have picked **Use for media** in Step 4.

### How to send logs

Device Portal → **File explorer** → **LocalAppData** → the folder that starts with `NSPX.NXbox` → **LocalState**. Download `nxbox_crash.txt` and `eden_uwp_diag.txt` and post them in the **#bugs** channel on [Discord](https://discord.gg/2SxEDzVyJM), with the game, the console (X or S) and what happened.

Did it run or not? Tell us in the **#compatibility** channel on Discord: one post per game, with the fps. The Series S runs it, but the author doesn't have one to test on, so your reports matter a lot.
