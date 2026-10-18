# Guia do NXbox (Português)

[English version below](#nxbox-guide-english)

Este guia leva você do zero até o primeiro jogo rodando. Ele é para quem nunca usou o Device Portal. São três partes, **em ordem**: instalar o NXbox, preparar os seus arquivos e jogar. Na Parte 2 você escolhe **um** de dois jeitos: o guiado, com o NxbHelper (recomendado), ou o manual.

O mesmo guia, mais curto, está no canal **#como-instalar** do [Discord](https://discord.gg/2SxEDzVyJM).

## Downloads

Tudo está na [página do último release](https://github.com/NspxMiguel/NXbox/releases/latest):

- **NxbHelper** (para o PC): `NxbHelper-windows-x64.zip`, `NxbHelper-macos-arm64.zip` ou `NxbHelper-linux-x64.zip`.
- **NXbox** (para o Xbox): `NXbox_game_…_x64.appx`.

## Antes de começar

Você precisa de:

- Um Xbox Series X ou S em **modo desenvolvedor** (app Xbox Dev Mode).
- Um PC na mesma rede do Xbox.
- Um HD ou pendrive USB 3.0 formatado em **exFAT** (o NXbox também detecta drives em NTFS). Não precisa de drive se você mandar tudo pelo Device Portal (veja a Parte 2).
- Os **seus** dumps, tirados do **seu** Switch: jogos (`.nsp`, `.nsz`, `.xci`), `prod.keys`, `title.keys` e o firmware.

O NXbox não vem com nada disso. Ninguém compartilha esses arquivos aqui.

Três regras para não quebrar nada:

1. Faça as partes na ordem: 1, 2, 3.
2. Na Parte 3, o Xbox pergunta o que fazer com o pendrive. **Nunca escolha "games & apps"**: isso formata o drive e apaga tudo.
3. Se algo sair diferente do que o guia diz que você vai ver, pare e vá para **Deu problema?** no fim.

## Parte 1 · Instale o NXbox

1. No Xbox, abra o **Dev Home** e ligue o **Device Portal** (Remote Access). Anote o endereço que aparece, tipo `https://192.168.x.x:11443`. Abra esse endereço no navegador do PC, ignore o aviso de conexão não segura (é normal no Device Portal) e entre com o login do Remote Access. Deu certo quando a página do Device Portal abre.
2. Em **Settings**, ligue **DefaultUWPContentTypeToGame**. Faça isso **antes** de instalar. Depois, na **Home**: **Add** → escolha o `NXbox_game_…_x64.appx` (baixado em **Downloads**) → **Next** → **Start**. Deu certo quando o NXbox aparece no Dev Home do Xbox.
3. No Dev Home: NXbox → **View details**. Tem que estar **App type: Game**. Como **App**, o NXbox ganha só 1 GB de memória e os jogos fecham com `bad allocation`. Se estiver como App, troque para Game ali mesmo; se não mudar, refaça o passo 2 e instale de novo.

## Parte 2 · Prepare os arquivos: escolha um jeito

Escolha **um** dos dois jeitos abaixo:

- [Jeito guiado, com o NxbHelper (recomendado)](#jeito-guiado-com-o-nxbhelper-recomendado)
- [Jeito manual](#jeito-manual)

Quando você usa um pendrive, ele tem que terminar em um destes layouts, na raiz do drive:

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

O layout A é o que o NxbHelper cria, e é bom para um HD usado por vários consoles. O B deixa as keys na raiz do pendrive e só `games` e `firmware` dentro de `NXbox/`.

### Jeito guiado, com o NxbHelper (recomendado)

Baixe o `.zip` do seu sistema (links em **Downloads**), descompacte e abra. No Mac e no Linux, na primeira vez, clique com o botão direito e escolha **Abrir**, ou rode `chmod +x` no arquivo.

O NxbHelper só prepara os arquivos: ele não instala o NXbox (isso é a Parte 1). Ele tem três opções:

- **Pelo pendrive.** Escolha os jogos, as keys (`prod.keys` e `title.keys`), o firmware e o pendrive. Ele confere tudo (as keys, o firmware contra as keys e downloads incompletos) e copia no layout A. O firmware pode estar solto, em subpastas, em `.zip` ou como a NAND (`rawnand.bin`): o NxbHelper descompacta sozinho.
- **Pelo Device Portal.** Manda tudo pela rede para o NXbox que você instalou na Parte 1, sem pendrive. É mais lento e enche o armazenamento do Xbox.
- **Conferir pendrive.** Olha um pendrive que você fez à mão e diz o que corrigir.

Como saber que deu certo: depois de copiar para o pendrive, rode **Conferir pendrive**. Se ele não apontar nenhum problema, siga para a Parte 3.

### Jeito manual

Faça tudo no PC, sem o NxbHelper:

1. Na raiz do pendrive, monte o layout A ou B (acima).
2. Em `games`, ponha os jogos, os updates e as DLC; as subpastas são opcionais.
3. Em `firmware`, ponha o dump em arquivos `.nca`/`.cnmt.nca`, subpastas ou `.zip`. Dumps NAND também são aceitos; NAND criptografada exige as BIS keys do seu console. Se o acesso ao dump falhar, prepare-o com o NxbHelper no PC.
4. Keys: o NXbox procura `prod.keys` e `title.keys` em `NXbox/`, depois em `switch/` e por último na raiz, nessa ordem. Elas têm que vir de um Switch com firmware **igual ou mais novo** que o firmware que você copiou.

Confira que **todos os arquivos terminaram de copiar ou baixar**: um arquivo incompleto causa erro. Como saber que deu certo: ao abrir o pendrive no PC, você vê um dos dois layouts e a pasta `firmware` está cheia de arquivos `.nca`.

## Parte 3 · Jogue

1. Plugue o pendrive no Xbox. Ele pergunta o que fazer com o drive: escolha **Use for media**. **Nunca escolha "games & apps": isso formata o drive e apaga tudo.** (Se você mandou tudo pelo Device Portal, pule este passo.)
2. Abra o NXbox. Se aparecer a tela **Pendrive detectado**, escolha **Usar como SSD externo**. Feche o NXbox (botão Xbox → **Sair**) e abra de novo, **uma vez**: é nessa segunda abertura que ele instala o firmware. Não desligue o console nem tire o pendrive enquanto isso. Deu certo quando a biblioteca mostra **Firmware instalado** e a versão.
3. Abra um jogo. Um `.nsz` é convertido na primeira vez: espere. Deu certo quando o jogo começa.

### Enviar pela rede ou liberar espaço interno

Em **Configurações → Online → Servidor FTP**, ative o servidor e use os dados mostrados. Envie jogos para `/interno/games/` ou direto para o HD em `/hd/games/`. [Configuração do FileZilla](nxbox-ftp.md).

Para mover um jogo já instalado internamente, abra **Detalhes** na biblioteca e aperte **X · Mover para o HD**, ao lado de apagar. O NXbox copia o jogo, as atualizações e DLC internos, mostra o progresso e confere os tamanhos antes de apagar os originais. Conecte o HD, aguarde a leitura da biblioteca, conclua downloads e desligue o FTP antes de mover. Arquivos existentes no destino ou espaço insuficiente bloqueiam a operação.

## Deu problema?

- **`bad allocation`:** o NXbox está como App. Volte à Parte 1, passo 3.
- **Ele pede firmware, ou dá erro ao instalar:** passe o pendrive pelo **Conferir pendrive** do NxbHelper. Keys mais velhas que o firmware não servem: tire as keys de novo de um Switch com firmware igual ou mais novo. O NXbox procura em `NXbox/firmware` e `firmware` na raiz de cada drive, além das pastas internas. Veja os caminhos exatos em **Configurações → Sistema** quando nenhum firmware for encontrado. O dump original é preservado após instalar.
- **Tela preta:** o firmware foi instalado? Se foi, mande os logs.
- **O pendrive não aparece:** ele tem que estar em exFAT ou NTFS, e você tem que ter escolhido **Use for media** (Parte 3, passo 1).

### Como mandar os logs

Device Portal → **File explorer** → **LocalAppData** → a pasta que começa com `NSPX.NXbox` → **LocalState**. Baixe `nxbox_crash.txt` e `eden_uwp_diag.txt` e poste os dois no canal **#bugs** do [Discord](https://discord.gg/2SxEDzVyJM), dizendo o jogo, o console (X ou S) e o que aconteceu.

Rodou? Conte no canal **#compatibilidade** do Discord, com o fps: um post por jogo. O Series S roda, mas o autor não tem um para testar, então os relatos de vocês valem muito.

---

# NXbox guide (English)

[Versão em português acima](#guia-do-nxbox-português)

This guide takes you from zero to your first game running. It is written for people who have never used the Device Portal. There are three parts, **in order**: install NXbox, prepare your files, and play. In Part 2 you pick **one** of two ways: the guided one with NxbHelper (recommended), or the manual one.

The same guide, shorter, is in the **#como-instalar** channel on [Discord](https://discord.gg/2SxEDzVyJM).

## Downloads

Everything is on the [latest release page](https://github.com/NspxMiguel/NXbox/releases/latest):

- **NxbHelper** (for the PC): `NxbHelper-windows-x64.zip`, `NxbHelper-macos-arm64.zip` or `NxbHelper-linux-x64.zip`.
- **NXbox** (for the Xbox): `NXbox_game_…_x64.appx`.

## Before you start

You need:

- An Xbox Series X or S in **Developer Mode** (Xbox Dev Mode app).
- A PC on the same network as the Xbox.
- A hard drive or USB 3.0 drive formatted as **exFAT** (NXbox also detects NTFS drives). You do not need a drive if you send everything over the Device Portal (see Part 2).
- **Your own** dumps from **your own** Switch: games (`.nsp`, `.nsz`, `.xci`), `prod.keys`, `title.keys` and the firmware.

NXbox ships with none of this. Nobody shares these files here.

Three rules so nothing breaks:

1. Do the parts in order: 1, 2, 3.
2. In Part 3, the Xbox asks what to do with the drive. **Never pick "games & apps"**: it formats the drive and erases everything.
3. If something looks different from what the guide says you will see, stop and go to **Something wrong?** at the end.

## Part 1 · Install NXbox

1. On the Xbox, open **Dev Home** and turn on the **Device Portal** (Remote Access). Note the address it shows, like `https://192.168.x.x:11443`. Open that address in your PC's browser, ignore the warning that the connection is not secure (it is normal for the Device Portal) and sign in with the Remote Access login. It worked when the Device Portal page opens.
2. Under **Settings**, turn on **DefaultUWPContentTypeToGame**. Do this **before** installing. Then, on **Home**: **Add** → pick `NXbox_game_…_x64.appx` (from **Downloads**) → **Next** → **Start**. It worked when NXbox shows up in the Xbox's Dev Home.
3. In Dev Home: NXbox → **View details**. It must say **App type: Game**. As an **App**, NXbox gets only 1 GB of memory and games close with `bad allocation`. If it says App, change it to Game right there; if it will not change, redo step 2 and install again.

## Part 2 · Prepare your files: pick one way

Pick **one** of the two ways below:

- [Guided way, with NxbHelper (recommended)](#guided-way-with-nxbhelper-recommended)
- [Manual way](#manual-way)

When you use a drive, it must end up in one of these layouts, at the drive root:

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

Layout A is what NxbHelper creates, and it suits a hard drive shared by several consoles. B keeps the keys at the drive root and only `games` and `firmware` inside `NXbox/`.

### Guided way, with NxbHelper (recommended)

Download the `.zip` for your system (links under **Downloads**), unzip it and open it. On Mac and Linux, the first time, right click and pick **Open**, or run `chmod +x` on the file.

NxbHelper only prepares files: it does not install NXbox (that is Part 1). It has three options:

- **With a USB drive.** Pick the games, the keys (`prod.keys` and `title.keys`), the firmware and the drive. It checks everything (the keys, the firmware against the keys, and incomplete downloads) and copies it in layout A. The firmware can be loose, in subfolders, a `.zip` or the NAND (`rawnand.bin`): NxbHelper unpacks it itself.
- **Over Device Portal.** Sends everything over the network to the NXbox you installed in Part 1, with no drive. It is slower and it fills the Xbox's storage.
- **Check USB drive.** Looks at a drive you made by hand and says what to fix.

How to tell it worked: after copying to the drive, run **Check USB drive**. If it reports no problem, go to Part 3.

### Manual way

Do everything on your PC, without NxbHelper:

1. At the drive root, build layout A or B (above).
2. In `games`, put the games, updates and DLC; subfolders are optional.
3. In `firmware`, put your dump as `.nca`/`.cnmt.nca` files, subfolders or a `.zip`. NAND dumps are also supported; encrypted NAND requires your console’s BIS keys. If dump access fails, prepare it with NxbHelper on a PC.
4. Keys: NXbox looks for `prod.keys` and `title.keys` in `NXbox/`, then in `switch/`, and last in the root, in that order. They must come from a Switch on the **same or newer** firmware than the one you copied.

Make sure **every file has finished copying or downloading**: an incomplete file causes an error. How to tell it worked: when you open the drive on your PC, you see one of the two layouts and the `firmware` folder is full of `.nca` files.

## Part 3 · Play

1. Plug the drive into the Xbox. It asks what to do with it: pick **Use for media**. **Never pick "games & apps": that formats the drive and erases everything.** (If you sent everything over the Device Portal, skip this step.)
2. Open NXbox. If the **USB drive detected** screen appears, pick **Use as an external SSD**. Close NXbox (Xbox button → **Quit**) and open it again, **once**: that second launch is when it installs the firmware. Do not turn off the console or unplug the drive meanwhile. It worked when the library shows **Installed firmware** and its version.
3. Open a game. An `.nsz` is converted the first time: wait. It worked when the game starts.

### Send over the network or free internal space

Enable **Settings → Online → FTP server** and use the displayed connection details. Send games to `/interno/games/` or straight to USB at `/hd/games/`. [FileZilla setup](nxbox-ftp.md).

To move an internally installed game, open **Details** in the library and press **X · Move to HD**, next to delete. NXbox copies the game and its internal updates/DLC, displays progress and verifies sizes before deleting originals. Connect the drive, wait for the library scan, finish downloads and turn off FTP first. Existing destination files or insufficient free space block the operation.

## Something wrong?

- **`bad allocation`:** NXbox is set as an App. Go back to Part 1, step 3.
- **It asks for firmware, or the install fails:** run the drive through NxbHelper's **Check USB drive**. Keys older than the firmware don't work: dump the keys again from a Switch on the same or newer firmware. NXbox searches `NXbox/firmware` and root-level `firmware` on each drive, plus internal folders. See the exact paths in **Settings → System** when no firmware is found. The original dump is preserved after installation.
- **Black screen:** did the firmware install? If so, send the logs.
- **NXbox doesn't find the drive:** it must be exFAT or NTFS, and you must have picked **Use for media** (Part 3, step 1).

### How to send logs

Device Portal → **File explorer** → **LocalAppData** → the folder that starts with `NSPX.NXbox` → **LocalState**. Download `nxbox_crash.txt` and `eden_uwp_diag.txt` and post both in the **#bugs** channel on [Discord](https://discord.gg/2SxEDzVyJM), with the game, the console (X or S) and what happened.

Did it run? Tell us in the **#compatibility** channel on Discord, with the fps: one post per game. The Series S runs it, but the author doesn't have one to test on, so your reports matter a lot.

Organize se quiser: games\Base (jogo), games\Updates (atualizações), games\DLC (DLCs). Tudo solto em games\ também funciona.

Optionally organize: games\Base (game), games\Updates (updates), games\DLC (DLC). Everything loose in games\ works too.
