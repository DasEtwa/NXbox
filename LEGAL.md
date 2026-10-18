# Legal

NXbox is an independent, open-source emulator. This page describes what the project ships, what it
does not ship, and the rules that apply to it and its community. It is not legal advice, and the
laws on emulation and personal copies differ from country to country.

## Not affiliated

NXbox is not affiliated with, endorsed by or sponsored by Nintendo, Microsoft, the Eden team or any
game publisher. Nintendo Switch, Xbox, Xbox Series X|S and every game name mentioned in this
repository are trademarks of their owners. They are used only to say what NXbox runs on and what it
has been tested with.

## What NXbox ships

Only its own code and the open-source code it is built from: the Eden emulator, its libraries,
Mesa and the tools listed in [NOTICE.md](NOTICE.md), each under its own license.

NXbox does **not** ship, and the project will never provide or link to:

- games, demos, updates or DLC;
- Switch firmware or system archives;
- keys (`prod.keys`, `title.keys`) or any other cryptographic material from a console;
- any Nintendo code.

The **Sources** screen starts empty. Any source in it is one you added. NXbox does not suggest or
bundle any.

## What you need to bring

Your own dumps, made from a Nintendo Switch you own and from games you bought: the game files,
firmware and keys. You make them with your own console and your own tools. You are responsible for
following the law where you live.

## Piracy is not supported

- Asking for or sharing games, firmware, keys, or links to them is not allowed anywhere the project
  runs: issues, pull requests, discussions and the Discord server. Such content is removed, and
  repeat offenders are banned. The Discord server blocks those requests automatically.
- A source added to **Sources** must be one you have the right to use, such as a server holding
  your own dumps. Do not post source addresses in the project's spaces.
- Help is only given for setups that use your own dumps.

## Content fetched at runtime

Some screens download data from other projects at the moment you ask for it: cheats from
[nx-cheats-db](https://github.com/sthetix/nx-cheats-db), mods from
[GameBanana](https://gamebanana.com) and from the
[nx-links](https://github.com/gamemoddesignbr/nx-links) index, and box art from
[LaunchBox Games Database](https://gamesdb.launchbox-app.com). NXbox does not host that content. It
belongs to its authors and is governed by their terms; see [NOTICE.md](NOTICE.md) for the credits.

## Xbox Developer Mode

NXbox runs only in Xbox Developer Mode, which Microsoft provides under its own agreement. NXbox is
not a Microsoft product and is not distributed through the Microsoft Store. Activating and using
Developer Mode is between you and Microsoft.

## Privacy

What the app and NxHelper connect to, what they store and how to delete it: [PRIVACY.md](PRIVACY.md).

## Donations

Donations are optional and buy nothing. There are no paid builds, no early access, no
donor-only features and no promise about any game.

## License and source code

NXbox is free software under the GNU GPL, version 3 or later; see [LICENSE.txt](LICENSE.txt).
Third-party licenses are in [LICENSES/](LICENSES/). Every release is built by GitHub Actions from
the tagged commit of this repository, so the complete corresponding source of each release is that
tag. Redistribution must keep the attribution described in [NOTICE.md](NOTICE.md).

## No warranty

NXbox is provided "as is", without warranty of any kind, as stated in sections 15 and 16 of the
GPL. It can crash, corrupt saves or misbehave. Keep backups of your saves.

## Contact

Write to **miguel@nspx.dev** if you believe something in this repository or its releases infringes
your rights. Say what it is and where it is, and it will be reviewed and removed if it does.
