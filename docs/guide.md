# Guia do NXbox (Português)

[English version below](#nxbox-guide-english)

Este guia leva você do zero até o primeiro jogo rodando. Ele é para quem nunca usou o Device Portal. Siga os passos **na ordem**. Não pule nenhum.

O mesmo guia, mais curto, está no canal **#como-instalar** do [Discord](https://discord.gg/2SxEDzVyJM).

## Downloads

Tudo está na [página do último release](https://github.com/NspxMiguel/NXbox/releases/latest):

- **NxHelper** (para o PC): `NxHelper-windows-x64.zip`, `NxHelper-macos-arm64.zip` ou `NxHelper-linux-x64.zip`.
- **NXbox** (para o Xbox): `NXbox_game_…_x64.appx`.

## Antes de começar

Você precisa de:

- Um Xbox Series X ou S em **modo desenvolvedor** (app Xbox Dev Mode, conta de desenvolvedor).
- Um PC ou celular na mesma rede, para abrir o Device Portal.
- Um HD ou pendrive USB 3.0 formatado em **exFAT** (o NXbox também detecta drives em NTFS).
- Os **seus** dumps, tirados do **seu** Switch: jogos (`.nsp`, `.nsz`, `.xci`), `prod.keys`, `title.keys` e o firmware.

O NXbox não vem com nada disso. Ninguém compartilha esses arquivos aqui.

Três regras para não quebrar nada:

1. Faça os passos na ordem.
2. No Passo 4, o Xbox pergunta o que fazer com o pendrive. **Nunca escolha "games & apps"**: isso apaga tudo.
3. Se algo sair diferente do que o guia diz que você vai ver, pare e vá para **Deu problema?** no fim.

## Passo 1: prepare o pendrive no PC

Há dois caminhos, e os dois terminam com o pendrive no mesmo formato: o jeito guiado, com o NxHelper (recomendado), ou à mão.

### Jeito guiado: NxHelper (Windows, Mac, Linux)

Baixe o `.zip` do seu sistema (links em **Downloads**), descompacte e abra. No Mac e no Linux, na primeira vez, clique com o botão direito e escolha **Abrir**, ou rode `chmod +x` no arquivo.

O NxHelper tem três opções:

- **Pelo pendrive (recomendado).** Escolha os jogos, o `prod.keys`, o `title.keys`, o firmware e o pendrive. Ele confere tudo (as keys, a versão do firmware contra a das keys e downloads incompletos) e copia cada coisa para a pasta certa. Aceita firmware solto, em subpastas, em `.zip` ou como backup completo da NAND (`rawnand.bin`), e descompacta sozinho.
- **Pelo Device Portal.** Manda os jogos pela rede direto para o NXbox já instalado. É mais lento e enche o armazenamento interno do Xbox. Só dá para usar depois do Passo 2.
- **Conferir pendrive.** Confere um pendrive que você preparou à mão e diz o que corrigir.

O NxHelper **não instala** o NXbox: isso são os Passos 2 e 3.

Como saber que deu certo: depois de preparar o pendrive, rode **Conferir pendrive**. Se ele não apontar nenhum problema, siga para o Passo 2.

### Jeito manual (alternativa)

Monte o pendrive no PC com um destes dois layouts, na raiz do drive:

```
A) NXbox/
   ├─ prod.keys
   ├─ title.keys
   ├─ games
   └─ firmware

B) raiz (root)
   ├─ prod.keys
   ├─ title.keys
   └─ NXbox/
      ├─ games
      └─ firmware
```

- **Layout A:** tudo dentro de `NXbox/`. É o que o NxHelper cria. É o ideal para um HD que vai ser usado por vários consoles.
- **Layout B:** as keys na raiz do pendrive, e só `games` e `firmware` dentro de `NXbox/`.
- **Games:** em `games` vão os jogos, os updates e as DLC, tudo junto.
- **Keys:** o NXbox procura `prod.keys` e `title.keys` em `NXbox/`, depois em `switch/` e por último na raiz, nessa ordem. As keys vêm de um Switch com firmware **igual ou mais novo** que o firmware que você copiou.
- **Firmware:** à mão, o Xbox lê só arquivos `.nca` soltos em `NXbox/firmware` ou dentro de uma subpasta dela. Um `.zip` precisa estar descompactado (ou passar pelo NxHelper).
- Confira que **todos os arquivos terminaram de copiar ou baixar**. Um arquivo incompleto causa erro.

Como saber que deu certo: ao abrir o pendrive no PC, você vê um dos dois layouts e a pasta `firmware` está cheia de arquivos `.nca`.

## Passo 2: instale o NXbox

1. No Xbox, abra o **Dev Home** e ligue o **Device Portal** (Remote Access). Anote o endereço que aparece, tipo `https://192.168.x.x:11443`.
2. Abra esse endereço no navegador do PC (ignore o aviso de conexão não segura: é normal no Device Portal) e entre com o login do Remote Access. Deu certo quando a página do Device Portal abre.
3. Em **Settings**, ligue **DefaultUWPContentTypeToGame**. Faça isso **antes** de instalar. Deu certo quando a opção aparece ligada.
4. Baixe o `NXbox_game_…_x64.appx` do [último release](https://github.com/NspxMiguel/NXbox/releases/latest).
5. Na **Home** do Device Portal: **Add** → escolha o `.appx` → **Next** → **Start**. Deu certo quando o NXbox aparece no Dev Home do Xbox.

## Passo 3: confira se é Game

No Dev Home: NXbox → **View details** → **App type**. Tem que estar **Game**.

Como **App**, o NXbox ganha só 1 GB de memória e os jogos fecham com `bad allocation`. Se estiver como **App**, troque para **Game** ali mesmo. Se não mudar, refaça o Passo 2.3 e instale de novo.

## Passo 4: plugue o pendrive no Xbox

O Xbox pergunta o que fazer com o pendrive. Escolha **Use for media**.

**Nunca escolha "games & apps": isso formata o drive e apaga tudo.**

Deu certo quando o Xbox deixa de perguntar e os arquivos continuam no pendrive.

## Passo 5: abra o NXbox

1. Na tela **Pendrive detectado**, escolha **Usar como SSD externo**.
2. Feche o NXbox (botão Xbox → **Sair**) e abra de novo, **uma vez**: é nessa segunda abertura que ele instala o firmware. Não desligue o console nem tire o pendrive enquanto isso. Deu certo quando a biblioteca mostra **Firmware instalado** e a versão. A pasta do firmware no pendrive muda de nome para `firmware_installed`: isso é normal.
3. Abra um jogo. Um `.nsz` é convertido na primeira vez: espere. Deu certo quando o jogo começa.

## Deu problema?

Se você usou o NxHelper, rode **Conferir pendrive** primeiro. Ele aponta keys, versão do firmware, downloads incompletos e pastas erradas.

- **O jogo fecha na hora ou aparece `bad allocation`:** o NXbox está como App. Volte ao Passo 3.
- **Ele pede firmware:** com o NxHelper serve firmware em `.nca` ou `.cnmt.nca` soltos, em subpastas, em `.zip` ou em `rawnand.bin`. À mão, o Xbox lê só `.nca` soltos em `NXbox/firmware` ou em uma subpasta; um `.zip` só funciona descompactado (ou passando pelo NxHelper). Depois de instalado, a pasta muda de nome para `firmware_installed`. Se o NXbox pedir o firmware de novo (por exemplo, depois de desinstalar e instalar outra vez), volte o nome para `firmware`.
- **Erro ao instalar o firmware:** as suas keys são mais velhas que esse firmware. Tire as keys de novo.
- **Tela preta:** confira se o firmware foi instalado (Passo 5.2). Se foi, mande os logs.
- **O pendrive não aparece:** ele tem que estar em exFAT ou NTFS, e você tem que ter escolhido **Use for media** no Passo 4.

### Como mandar os logs

Device Portal → **File explorer** → **LocalAppData** → a pasta que começa com `NSPX.NXbox` → **LocalState**. Baixe `nxbox_crash.txt` e `eden_uwp_diag.txt` e poste os dois no canal **#bugs** do [Discord](https://discord.gg/2SxEDzVyJM), dizendo o jogo, o console (X ou S) e o que aconteceu.

Rodou? Conte no canal **#compatibilidade** do Discord, com o fps: um post por jogo. O Series S roda, mas o autor não tem um para testar, então os relatos de vocês valem muito.

---

# NXbox guide (English)

[Versão em português acima](#guia-do-nxbox-português)

This guide takes you from zero to your first game running. It is written for people who have never used the Device Portal. Follow the steps **in order**. Do not skip any.

The same guide, shorter, is in the **#como-instalar** channel on [Discord](https://discord.gg/2SxEDzVyJM).

## Downloads

Everything is on the [latest release page](https://github.com/NspxMiguel/NXbox/releases/latest):

- **NxHelper** (for the PC): `NxHelper-windows-x64.zip`, `NxHelper-macos-arm64.zip` or `NxHelper-linux-x64.zip`.
- **NXbox** (for the Xbox): `NXbox_game_…_x64.appx`.

## Before you start

You need:

- An Xbox Series X or S in **Developer Mode** (Xbox Dev Mode app, developer account).
- A PC or phone on the same network, to open the Device Portal.
- A hard drive or USB 3.0 drive formatted as **exFAT** (NXbox also detects NTFS drives).
- **Your own** dumps from **your own** Switch: games (`.nsp`, `.nsz`, `.xci`), `prod.keys`, `title.keys` and the firmware.

NXbox ships with none of this. Nobody shares these files here.

Three rules so nothing breaks:

1. Do the steps in order.
2. In Step 4, the Xbox asks what to do with the drive. **Never pick "games & apps"**: it erases everything.
3. If something looks different from what the guide says you will see, stop and go to **Something wrong?** at the end.

## Step 1: prepare the drive on your PC

There are two ways, and both end with the drive in the same layout: the guided way with NxHelper (recommended), or by hand.

### Guided way: NxHelper (Windows, Mac, Linux)

Download the `.zip` for your system (links under **Downloads**), unzip it and open it. On Mac and Linux, the first time, right click and pick **Open**, or run `chmod +x` on the file.

NxHelper has three options:

- **With a USB drive (recommended).** Pick the games, `prod.keys`, `title.keys`, the firmware and the drive. It checks everything (the keys, the firmware version against the keys, and incomplete downloads) and copies each item into the right folder. It takes firmware loose, in subfolders, as a `.zip` or as a full NAND backup (`rawnand.bin`), and unpacks it itself.
- **Over Device Portal.** Sends the games over the network straight to the NXbox that is already installed. It is slower and it fills the Xbox's internal storage. It only works after Step 2.
- **Check USB drive.** Checks a drive you prepared by hand and says what to fix.

NxHelper **does not install** NXbox: that is Steps 2 and 3.

How to tell it worked: after preparing the drive, run **Check USB drive**. If it reports no problem, go to Step 2.

### Manual way (alternative)

Set up the drive on your PC with one of these two layouts, at the drive root:

```
A) NXbox/
   ├─ prod.keys
   ├─ title.keys
   ├─ games
   └─ firmware

B) root
   ├─ prod.keys
   ├─ title.keys
   └─ NXbox/
      ├─ games
      └─ firmware
```

- **Layout A:** everything inside `NXbox/`. It is what NxHelper creates. It is ideal for a hard drive shared by several consoles.
- **Layout B:** the keys at the drive root, and only `games` and `firmware` inside `NXbox/`.
- **Games:** `games` takes games, updates and DLC, all together.
- **Keys:** NXbox looks for `prod.keys` and `title.keys` in `NXbox/`, then in `switch/`, and last in the root, in that order. The keys come from a Switch on the **same or newer** firmware than the one you copied.
- **Firmware:** by hand, the Xbox reads only loose `.nca` files in `NXbox/firmware` or inside one subfolder of it. A `.zip` must be unpacked first (or go through NxHelper).
- Make sure **every file has finished copying or downloading**. An incomplete file causes an error.

How to tell it worked: when you open the drive on your PC, you see one of the two layouts and the `firmware` folder is full of `.nca` files.

## Step 2: install NXbox

1. On the Xbox, open **Dev Home** and turn on the **Device Portal** (Remote Access). Note the address it shows, like `https://192.168.x.x:11443`.
2. Open that address in your PC's browser (ignore the warning that the connection is not secure: it is normal for the Device Portal) and sign in with the Remote Access login. It worked when the Device Portal page opens.
3. Under **Settings**, turn on **DefaultUWPContentTypeToGame**. Do this **before** installing. It worked when the option shows as on.
4. Download `NXbox_game_…_x64.appx` from the [latest release](https://github.com/NspxMiguel/NXbox/releases/latest).
5. On the Device Portal **Home**: **Add** → pick the `.appx` → **Next** → **Start**. It worked when NXbox shows up in the Xbox's Dev Home.

## Step 3: check that it is a Game

In Dev Home: NXbox → **View details** → **App type**. It must say **Game**.

As an **App**, NXbox gets only 1 GB of memory and games close with `bad allocation`. If it says **App**, change it to **Game** right there. If it will not change, redo Step 2.3 and install again.

## Step 4: plug the drive into the Xbox

The Xbox asks what to do with the drive. Pick **Use for media**.

**Never pick "games & apps": that formats the drive and erases everything.**

It worked when the Xbox stops asking and your files are still on the drive.

## Step 5: open NXbox

1. On the **USB drive detected** screen, pick **Use as an external SSD**.
2. Close NXbox (Xbox button → **Quit**) and open it again, **once**: that second launch is when it installs the firmware. Do not turn off the console or unplug the drive meanwhile. It worked when the library shows **Installed firmware** and its version. The firmware folder on the drive is renamed to `firmware_installed`: this is normal.
3. Open a game. An `.nsz` is converted the first time: wait. It worked when the game starts.

## Something wrong?

If you used NxHelper, run **Check USB drive** first. It points out keys, firmware version, incomplete downloads and wrong folders.

- **The game closes right away, or `bad allocation` appears:** NXbox is set as an App. Go back to Step 3.
- **It asks for firmware:** with NxHelper, firmware as loose `.nca` or `.cnmt.nca` files, in subfolders, as a `.zip` or as `rawnand.bin` all work. By hand, the Xbox reads only loose `.nca` files in `NXbox/firmware` or in one subfolder; a `.zip` only works unpacked (or given to NxHelper). Once installed, the folder is renamed to `firmware_installed`. If NXbox asks for the firmware again (for example after you uninstalled and reinstalled it), rename the folder back to `firmware`.
- **Firmware install error:** your keys are older than that firmware. Dump the keys again.
- **Black screen:** check that the firmware was installed (Step 5.2). If it was, send the logs.
- **NXbox doesn't find the drive:** it must be exFAT or NTFS, and you must have picked **Use for media** in Step 4.

### How to send logs

Device Portal → **File explorer** → **LocalAppData** → the folder that starts with `NSPX.NXbox` → **LocalState**. Download `nxbox_crash.txt` and `eden_uwp_diag.txt` and post both in the **#bugs** channel on [Discord](https://discord.gg/2SxEDzVyJM), with the game, the console (X or S) and what happened.

Did it run? Tell us in the **#compatibility** channel on Discord, with the fps: one post per game. The Series S runs it, but the author doesn't have one to test on, so your reports matter a lot.
