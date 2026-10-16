# Community backgrounds

Run `python3 tools/nxbox/build_art_index.py` from the repository root to generate
`backgrounds.json` from the keyless [LaunchBox Games Database](https://gamesdb.launchbox-app.com/).
For an offline export, pass `--metadata /path/to/Metadata.zip` (or Metadata.xml).
Only Nintendo Switch Fanart - Background and Banner filenames are distributed;
no image bytes or API credentials belong here. The weekly `art-index.yml` workflow
regenerates and commits the compact index on the default branch.

Images are community contributions, credited in NXbox Settings > Credits. Covers
come from the user's own game packages; unavailable backgrounds use the blurred icon.
