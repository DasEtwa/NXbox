# Contributing to NXbox

NXbox is a fork of [Eden](https://git.eden-emu.dev/eden-emu/eden) for Xbox Series X|S. Send bug
reports and pull requests here, not to Eden: changes in this repository are not upstream changes.

## Reporting a bug

Open an issue at <https://github.com/NspxMiguel/NXbox/issues>, one issue per bug. English or
Portuguese is fine. Include:

- the game, its title ID and version (base game, update, DLC);
- Series S or Series X;
- the NXbox version you installed (for example 1.0.4);
- what you did and what happened, and a photo or video if it is visual;
- `eden_uwp_diag.txt` and `nxbox_crash.txt` from the app's `LocalState` folder. Copy them right
  after the problem: the next game you open overwrites the diagnostic file.

## Pull requests

Pull requests are welcome. Every one is reviewed and tested on a console before it is merged.

- Keep a pull request to one change, and say in the description what it fixes and how you tested it.
- Code, comments and commit messages are in English.
- Format with the repository config (`src/.clang-format` for C++).
- Follow the style of the surrounding code; the Xbox-specific parts live under
  `src/video_core/renderer_d3d12`, `src/eden_uwp`, `src/nxbox_launcher` and `tools/nxbox`, and `docs/nxbox-port.md`
  describes the port.

## License

NXbox is licensed under the GPLv3, like Eden. By sending a contribution you agree to license it
under the same terms.
