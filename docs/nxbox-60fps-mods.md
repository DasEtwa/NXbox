# Community 60 FPS mods on NXbox

NXbox loads user-supplied executable patches and Atmosphere cheats at game startup.
No community patches, cheats, or game files are shipped with NXbox. Obtain a patch
from its author and follow that author's game-version and gameplay instructions.

A 60 FPS patch removes a game's own limit; it does not make the emulator render
faster. A game limited to 30 FPS can reach 60 only when CPU and GPU work fit within
16.7 ms per frame. Games already running below 30 FPS need performance improvements
as well. Some patches also need timing or physics fixes supplied by their author.

## Put the files here

Use the app's LocalState folder (accessible through NXbox's file tools):

```text
LocalState/
  load/
    0100123456789000/             # the game's 16-digit title ID
      Community 60 FPS/          # any descriptive mod name
        exefs/
          <NSO-BUILD-ID>.ips     # IPS or IPS32 binary format
          <NSO-BUILD-ID>.ips32   # also accepted
          unlock.pchtxt         # IPSwitch text, with its own @nsobid
```

Use only the files the author provides; the names above illustrate the supported
formats, not three files you must install together. Filename extensions and hex
letters may be upper or lower case. An IPS filename must contain the executable's
Build ID, not the title ID. An IPSwitch filename is arbitrary: its `@nsobid` selects
the executable. The patch must match the Build ID of the installed game update.
NXbox does not apply a patch to another Build ID. IPSwitch `@enabled` sections run;
`@disabled` sections remain disabled.

You can keep the same `load/<title-id>/<mod>/...` tree beside the game on the
external drive, for example:

```text
E:/NXbox/games/
  MyGame.nsp
  load/0100123456789000/Community 60 FPS/exefs/<NSO-BUILD-ID>.ips
```

NXbox searches the external game folders and the selected game's containing folder.
For a game in `E:/MyGame/MyGame.xci`, use
`E:/MyGame/load/<title-id>/<mod>/exefs/`. No copying to internal storage is required.
The existing `LocalState/eden/load/<title-id>/<mod>/...` location used by the mod and
cheat stores also remains supported.

## Atmosphere cheats

For a 60 FPS unlock distributed as an Atmosphere cheat, use:

```text
load/<title-id>/Community 60 FPS/cheats/<BUILD-ID-FIRST-16-HEX-DIGITS>.txt
```

The text file contains the author's `[Cheat name]` blocks, opcode lines and any
required `{Master code}` block. Every valid block in a manually installed file is
enabled by default. Install only the blocks you intend to run: do not copy a whole
database containing mutually exclusive 30/60 FPS or resolution options. The cheat
store writes the blocks selected in its UI to the legacy `eden/load` location.

## Enable, disable and verify

Restart the game after installing or changing a mod. A present mod is enabled by
default unless its folder name is in the game's disabled add-ons list. The mod
store's saved disabled choices still apply. Remove the mod folder to disable a
manually installed mod, or set `NXBOX_MODS=0` in `LocalState/nxbox_env.txt` to disable
all executable, layered and cheat mods for diagnosis. Restart the game after
changing this setting; remove the line or set it to `1` to enable mods again.

Use one FPS patch for the active game version. Do not install duplicate copies
in different locations. The existing `eden/load` root takes precedence over
`LocalState/load`, followed by external roots; mods run in folder-name order and
patch files in filename order. A later conflicting write can replace an earlier
one. Identical mod paths are merged with the earlier root taking precedence.

Check the game log for:

- `MODS load root`: the additional folders NXbox opened.
- `NXBOX_MOD_APPLIED`: the title, executable, Build ID and patch path successfully
  processed by the patch loader.
- `NXBOX_MOD_FAILED`: a binary patch could not be processed; the previous image
  was retained.
- `NXBOX_CHEAT_LOADED`: each enabled Atmosphere block handed to the cheat engine.

A patch-loader success means the file was processed, not proof that the game
reached 60 FPS or that every IPSwitch record was in range. Check FPS and gameplay.
If no patch is applied, check title ID, active update/Build ID, directory nesting,
disabled choices and the global opt-out. For pchtxt, also check the author's
`@enabled` sections. Keep the author's timing fixes and required companion mods.
