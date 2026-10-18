// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

export default {
  transferDone: "Files sent. Open NXbox on your Xbox to import firmware and play.",
  driveProd: "Console keys",
  driveTitle: "Title keys",
  driveFirmware: "Firmware",
  driveGames: "Games found: {count}",
  driveLayout: "Game outside the library: {path}",
  driveFound: "Found at {path}",
  driveKeysFix: "Copy a valid prod.keys (with a 32-byte header_key) into NXbox/ or the drive root.",
  driveTitleMissing:
    "Optional; only needed for some games. Copy title.keys into NXbox/ or the drive root.",
  driveTitleInvalid: "Copy a valid title.keys into NXbox/ or the drive root.",
  driveFirmwareOk: "Version {version}, {count} files",
  driveFirmwareMissing: "Copy your whole firmware dump to NXbox/firmware/.",
  driveUnpack: "NxHelper can unpack it. Choose Fix it.",
  driveOldKeys: "The keys are older than the firmware. Dump keys again from your updated Switch.",
  driveFirmwareCopy: "Copy the whole firmware dump again.",
  driveChunksMove:
    "These NCA chunk folders sit where NxHelper writes the finished files. Move the whole firmware dump to firmware/ at the drive root, then choose Fix it.",
  driveNandMany:
    "Could not pick one NAND backup: more than one was found, or its parts (00, 01, …) are incomplete. Keep a single emuMMC folder with all its parts.",
  driveRename: "Rename the extension to lowercase so Xbox can list it.",
  driveConvert: "Convert .xcz to .xci before copying; Xbox cannot list .xcz.",
  driveTruncated: "Incomplete game. Finish the download or copy the complete dump again.",
  driveNoId: "No title ID in the filename. Add [0100…] with the game title ID.",
  driveMove: "Move the game to games/ or NXbox/games/ at the drive root.",
  driveNoGames:
    "No games on the drive yet. Put them in NXbox/games/ (games/ at the root also works).",
  driveReady: "Ready to plug into the Xbox",
  driveNotReady: "Fix the failed items before plugging into the Xbox.",
  driveConflict:
    "A different file has the same name. Rename it or choose another destination; nothing was copied.",
  driveSpace: "Not enough free space. Free space or use another drive.",
  consoleFull:
    "The Xbox internal storage is full. Delete games you no longer use in NXbox or use a USB drive as external storage.",
  consoleFullSpace:
    "The Xbox internal storage is full: {size} more is needed. Delete games you no longer use in NXbox or use a USB drive as external storage.",
  nxboxMissing:
    "NXbox is not installed. Install NXbox from https://github.com/NspxMiguel/NXbox/releases/latest first.",
  xboxSpaceWarning:
    "Console free space is unavailable. Open NXbox once so it can report its free space.",
  driveUsage:
    "nxhelper --check-drive <path> [--fix]\nnxhelper --to-drive <path> <files…> --keys <prod.keys> [--title-keys <title.keys>] [--firmware <path>]\nnxhelper --to-xbox [--host <host>] <files…> --keys <prod.keys> [--title-keys <title.keys>] [--firmware <path>]",
  wayChoice:
    "1 — With a USB drive (recommended)\n2 — Over Device Portal\n3 — Install a game as its own app\nChoose a way",
  drivePath: "USB drive folder",
  gamePaths: "Game path (repeat for more games; empty to finish)",

  profileUsage:
    "Usage: nxhelper profile multi on|off, or nxhelper profile avatar <name> <image> | --character <id> --color <name> [--insecure-portal]",
  profilePackage: "Package full name (choose the app whose accounts you want to edit)",
  profileMissing:
    "Profile not found, duplicate name, or invalid profile data. Open NXbox once to create its profiles.",
  profileClose:
    "Close the selected NXbox app on the console before continuing. Press Enter when closed.",
  profileDone: "Profile updated. Reopen the selected NXbox app.",
  avatarInvalid:
    "Choose a valid PNG, JPEG, WebP, GIF or BMP image (up to 20 MB). The browser must finish within two minutes.",
  avatarBrowser: "Converting in your local browser. If it did not open, open this URL on this PC:",
  avatarConverting: "Preparing your NXbox avatar…",
  avatarConverted: "Image prepared. Return to NxHelper; you can close this tab.",

  guide: "Drag the game file into this window and press Enter (supported: .nsp .xci .nsz .xcz)",
  legal: "You must own the game and dump your own keys. NxHelper never downloads games or keys.",
  keys: "Drag your prod.keys into this window and press Enter",
  extra: "Drag an update/DLC file, or press Enter to continue (one file at a time)",
  host: "Xbox address (Dev Home > Remote Access)",
  found: "Xbox candidates found",
  user: "Device Portal username",
  password: "Device Portal password (hidden)",
  saved: "Use credentials from the OS secure store? [Y/n]",
  remember: "Save credentials in the OS secure store? [y/N]",
  secureUnavailable: "Secure storage is unavailable; credentials are kept only for this session.",
  fallback:
    "Control NCA metadata extraction is not implemented. Using the filename title ID/name and a generic NXbox icon.",
  title: "Base game title ID (16 hexadecimal digits starting with 0100)",
  name: "Game display name",
  runtime: "Downloading and verifying the NXbox runtime…",
  install: "Registering your independent game app…",
  upload: "Uploading to this app's private LocalState…",
  done: "Ready. Open this game's tile in Xbox Dev Home; set its app type to Game for the full memory budget.",
  failed: "NxHelper failed",
  file: "Choose an existing, nonempty .nsp, .xci, .nsz or .xcz file.",
  keyFile: "Choose an existing, nonempty file named prod.keys.",
  titleInvalid:
    "A base game title ID must contain 16 hex digits, begin with 0100 and end with 000.",
  nameInvalid: "Enter a game name of 1–64 characters without control characters.",
  compressed:
    "NSZ/XCZ needs the optional nsz decompressor on PATH (nsz -D). Install it or provide an NSP/XCI dump.",
  runtimeMissing:
    "Runtime unavailable; CI downloads may require NXHELPER_GITHUB_TOKEN. Supply --runtime-url and --runtime-sha256 (CI artifact ZIP or game APPX).",
  runtimeInvalid: "Runtime download, checksum or payload validation failed.",
  portal: "Device Portal request failed. Check the address, credentials and Dev Mode.",
  deploy:
    "Xbox registration failed or timed out. Check VCLibs and the Device Portal deployment log.",
  interrupted: "Cancelled.",
  secret: "A terminal is required to enter a hidden password.",
  args: "Invalid arguments. Run --help.",
  progress: "Sending",
  itemGame: "Game",
  itemKeys: "Keys (prod.keys)",
  itemUpdate: "Update",
  itemDlc: "DLC",
  itemFirmware: "Firmware",
  itemSaves: "Save data",
  itemMods: "Mods",
  itemTitleKeys: "Keys (title.keys)",
  fileCount: "files",
  firmware:
    "Drag a firmware NCA folder, ZIP or hekate rawnand backup, or Enter to skip (recommended: without firmware, Mii screens and some system applets stay empty)",
  saves: "Drag a JKSV/Checkpoint backup leaf folder or ZIP for this title, or Enter to skip",
  mods: "Drag romfs/exefs folders or a mod folder/ZIP for this title, or Enter to skip",
  titleKeys: "Drag title.keys, or Enter to skip",
  inputInvalid:
    "Invalid folder/ZIP: unsafe paths, unsupported archive, empty data or corrupt filesystem.",
  firmware7z: "Extract the .7z first, then select the extracted firmware folder.",
  firmwareRar: "Extract the .rar first, then select the extracted firmware folder.",
  firmwarePartition:
    "Partition-based emuMMC is not supported. Use Hekate to back it up to files, then select the eMMC folder.",
  firmwareInvalid:
    "Firmware rejected: NCA filename SHA-256, encrypted NCA3 header or SystemVersion data is invalid.",
  firmwareIncomplete:
    "Firmware incomplete: requires SystemVersion (0100000000000809, Data) and Mii model (0100000000000802, Data/PublicData).",
  firmwareKeys:
    "Firmware requires header_key and, for NAND, bis_key_02 (32 bytes, crypt+tweak). Encrypted SystemVersion also requires the matching key_area_key in prod.keys.",
  nandInvalid:
    "Invalid hekate NAND: missing/ambiguous SYSTEM GPT partition, missing split parts or unreadable FAT32 (check bis_key_02).",
  optionalMismatch:
    "Rejected: backup/mod title ID does not match this game. Drop one backup leaf for this title.",
  modsInvalid:
    "Rejected: mods must contain romfs/exefs inside one mod folder or load/<titleid>/<modname>/.",
  titleKeysInvalid: "Choose an existing, nonempty file named title.keys.",
  saveExists:
    "Save import stopped: this app already has a profile. Use a fresh game app to avoid replacing its user or saves.",
  none: "-",
  rejectNoId:
    "Rejected: no single [title ID] token in the file name, so it cannot be matched to the game.",
  rejectMismatch:
    "Rejected: this title ID does not belong to the game (expected its update ID or DLC range).",
  rejectDuplicate: "Rejected: this file or title ID was already added.",
  help: "NxHelper\nUsage: nxhelper [--runtime-url HTTPS_URL --runtime-sha256 HEX] [--insecure-portal]\n--insecure-portal accepts the Xbox self-signed TLS certificate for this connection only.\nDefault runtime: latest NspxMiguel/NXbox release with a SHA-256 asset digest.\nNo games or keys are included in the staged runtime. NSZ/XCZ requires nsz on PATH.",
};
