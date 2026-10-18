// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
import { copyFile, lstat, mkdir, mkdtemp, readdir, rm, statfs } from "node:fs/promises";
import { constants } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, extname, join, relative, resolve, sep } from "node:path";
import { readKeys, prepareFirmware, verifyFirmware } from "./firmware";
import { fileReader, folderFiles, safePath, zipEntries, type LocalFile } from "./storage";
import { gameExtension, parseTitleId, validateFile } from "./files";
import { PackError, strings, type Language, type Message } from "./i18n";
import type { DevicePortal } from "./portal";
import { imageParts } from "./nand";
import { prepareContent } from "./content";

export type Status = "ok" | "warn" | "fail";
export type DriveItem = {
  id: string;
  status: Status;
  title: string;
  detail?: string;
  fix?: string;
};
export type DriveReport = { root: string; items: DriveItem[]; ready: boolean };
export type DriveInputs = {
  games: string[];
  keys: string;
  titleKeys?: string;
  firmware?: string;
  language?: Language;
};
export type Progress = (file: string, sent: number, total: number) => void;
// Tests can supply a synthetic verifier without replacing the production crypto implementation.
export type DriveServices = { verify: typeof verifyFirmware; prepare: typeof prepareFirmware };
const services: DriveServices = { verify: verifyFirmware, prepare: prepareFirmware };
function message(language: Language, key: Message, values: Record<string, string | number> = {}) {
  return Object.entries(values).reduce(
    (s, [k, v]) => s.replaceAll(`{${k}}`, String(v)),
    strings[language][key],
  );
}
// Paths shown to the user always use "/", the way the guide writes them, on every system.
const shown = (root: string, path: string) => relative(root, path).split(sep).join("/");
async function entries(path: string) {
  try {
    return await readdir(path, { withFileTypes: true });
  } catch (error) {
    if (["ENOENT", "ENOTDIR"].includes((error as NodeJS.ErrnoException).code ?? "")) return [];
    throw error;
  }
}
async function exists(path: string) {
  try {
    return (await lstat(path)).isFile();
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return false;
    throw error;
  }
}
function validTitleKeys(keys: Awaited<ReturnType<typeof readKeys>>) {
  return (
    keys.size > 0 &&
    [...keys].every(([name, value]) => /^[a-f0-9]{32}$/.test(name) && value.length === 16)
  );
}
async function keyPath(root: string, name: string) {
  // The Xbox's order: NXbox/ (all-in-one folder on a shared drive), switch/, then the root.
  for (const directory of ["NXbox", "switch", ""]) {
    const path = join(root, directory, name);
    if (await exists(path)) return path;
  }
}
async function misplacedKey(root: string, name: string) {
  for (const e of await entries(root))
    if (e.isDirectory()) {
      const path = join(root, e.name, name);
      if (await exists(path)) return path;
    }
}
async function firmwareSource(
  root: string,
): Promise<{ source?: string; files: LocalFile[]; fixable: boolean; unsupported?: Message }> {
  const nca = /(?:^|\/)[a-f0-9]{32}(?:\.cnmt)?\.nca(?:\/|$)/i;
  for (const path of [join(root, "NXbox/firmware"), join(root, "firmware")]) {
    if (!(await entries(path)).length) continue;
    const all = await folderFiles(path);
    const files = all.filter((f) => /\.nca$/i.test(f.name) && f.name.split("/").length <= 2);
    const loose = new Set(files.map((f) => basename(f.name).toLowerCase()));
    const needsUnpack = all.some((f) => {
      const match = f.name.match(nca);
      return match && !loose.has(match[0].replace(/^\/|\/$/g, "").toLowerCase());
    });
    if (needsUnpack) return { source: path, files: [], fixable: true };
    if (files.length) return { source: path, files, fixable: false };
    if (imageParts(all)) return { source: path, files: [], fixable: true };
  }
  for (const directory of [join(root, "NXbox"), root]) {
    // A file-based emuMMC can live at the drive root or under NXbox/.
    for (const child of ["emuMMC", "eMMC"]) {
      const path = join(directory, child);
      if ((await entries(path)).length) {
        const files = await folderFiles(path);
        if (imageParts(files, child === "eMMC")) return { source: path, files: [], fixable: true };
        if (files.some((f) => /(?:^|\/)(?:emummc\.ini|raw_based)$/i.test(f.name)))
          return { source: path, files: [], fixable: false, unsupported: "firmwarePartition" };
      }
    }
    for (const e of await entries(directory)) {
      if (!e.isFile()) continue;
      const path = join(directory, e.name);
      if (/^(?:rawnand\.bin(?:\.\d{2})?|SYSTEM(?:\.bin)?)$/i.test(e.name))
        return { source: path, files: [], fixable: true };
      if (/^firmware\.(7z|rar)$/i.test(e.name))
        return {
          source: path,
          files: [],
          fixable: false,
          unsupported: /\.7z$/i.test(e.name) ? "firmware7z" : "firmwareRar",
        };
      if (/^firmware\.zip$/i.test(e.name)) return { source: path, files: [], fixable: true };
      if (/\.zip$/i.test(e.name)) {
        try {
          const files = await zipEntries(path);
          if (files.some((f) => nca.test(f.name)) || imageParts(files))
            return { source: path, files: [], fixable: true };
        } catch {
          /* An unrelated unreadable ZIP is not a firmware source. */
        }
      }
    }
  }
  return { files: [], fixable: false };
}
export function hasFixableFirmware(report: DriveReport) {
  return report.items.some(
    (i) =>
      i.id === "firmware" &&
      i.status === "fail" &&
      Object.values(strings).some((table) => i.fix === table.driveUnpack),
  );
}

/** Validate the complete PFS0 extent without loading game payloads into memory. */
export async function validateGame(path: string) {
  await validateFile(path);
  if (![".nsp", ".nsz"].includes(gameExtension(path))) return;
  const r = await fileReader([path]);
  try {
    const h = await r.read(0, 16);
    if (h.toString("ascii", 0, 4) !== "PFS0") throw new Error("Invalid PFS0");
    const count = h.readUInt32LE(4),
      names = h.readUInt32LE(8);
    const start = 16 + count * 24 + names;
    if (count > 100000 || start > r.size) throw new Error("Invalid PFS0 extent");
    const table = await r.read(16, count * 24);
    let end = 0n;
    for (let i = 0; i < count; i++) {
      const offset = table.readBigUInt64LE(i * 24),
        size = table.readBigUInt64LE(i * 24 + 8);
      if (offset + size > end) end = offset + size;
    }
    if (BigInt(start) + end !== BigInt(r.size)) throw new Error("Incomplete PFS0");
  } catch (cause) {
    throw new PackError("driveTruncated", { cause });
  } finally {
    await r.close();
  }
}
export async function checkDrive(
  root: string,
  language: Language = "en",
  deps = services,
): Promise<DriveReport> {
  root = resolve(root);
  if (!(await lstat(root)).isDirectory()) throw new PackError("inputInvalid");
  const items: DriveItem[] = [];
  const add = (id: string, status: Status, title: Message, detail?: string, fix?: Message) =>
    items.push({
      id,
      status,
      title: message(language, title),
      ...(detail && { detail }),
      ...(fix && { fix: message(language, fix) }),
    });
  const keysPath = await keyPath(root, "prod.keys");
  let keys: Awaited<ReturnType<typeof readKeys>> | undefined;
  try {
    if (keysPath) keys = await readKeys(keysPath);
  } catch {
    /* Report unreadable keys below. */
  }
  const wrongKeys = keysPath ?? (await misplacedKey(root, "prod.keys"));
  add(
    "prodKeys",
    keys?.get("header_key")?.length === 32 ? "ok" : "fail",
    "driveProd",
    wrongKeys ? message(language, "driveFound", { path: shown(root, wrongKeys) }) : undefined,
    keys?.get("header_key")?.length === 32 ? undefined : "driveKeysFix",
  );
  const title = await keyPath(root, "title.keys");
  let validTitle = false;
  try {
    validTitle = !!title && validTitleKeys(await readKeys(title));
  } catch {
    /* Optional malformed keys are a warning. */
  }
  const wrongTitle = title ?? (await misplacedKey(root, "title.keys"));
  add(
    "titleKeys",
    validTitle ? "ok" : "warn",
    "driveTitle",
    wrongTitle ? message(language, "driveFound", { path: shown(root, wrongTitle) }) : undefined,
    validTitle ? undefined : title ? "driveTitleInvalid" : "driveTitleMissing",
  );
  const firmware = await firmwareSource(root);
  if (firmware.unsupported)
    add("firmware", "fail", "driveFirmware", shown(root, firmware.source!), firmware.unsupported);
  else if (firmware.fixable)
    add("firmware", "fail", "driveFirmware", shown(root, firmware.source!), "driveUnpack");
  else if (!firmware.files.length)
    add("firmware", "fail", "driveFirmware", undefined, "driveFirmwareMissing");
  else {
    try {
      const fw = await deps.verify(firmware.files, keys ?? new Map());
      add(
        "firmware",
        "ok",
        "driveFirmware",
        message(language, "driveFirmwareOk", { version: fw.version, count: fw.files.length }),
      );
    } catch (error) {
      add(
        "firmware",
        "fail",
        "driveFirmware",
        undefined,
        error instanceof PackError && error.code === "firmwareKeys"
          ? "driveOldKeys"
          : "driveFirmwareCopy",
      );
    }
  }
  const games: string[] = [];
  for (const directory of [join(root, "games"), join(root, "NXbox/games")])
    for (const e of await entries(directory))
      if (e.isFile() && /\.(nsp|nsz|xci|xcz)$/i.test(e.name)) games.push(join(directory, e.name));
  items.push({
    id: "games",
    status: games.length ? "ok" : "fail",
    title: message(language, "driveGames", { count: games.length }),
    ...(!games.length && { fix: message(language, "driveMove") }),
  });
  for (const path of games) {
    const extension = gameExtension(path);
    const problems: { id: string; status: Status; fix: Message }[] = [];
    if (extension === ".xcz" || extname(path) !== extension)
      problems.push({
        id: "gameExtension",
        status: "warn",
        fix: extension === ".xcz" ? "driveConvert" : "driveRename",
      });
    try {
      await validateGame(path);
    } catch {
      problems.unshift({ id: "gameIncomplete", status: "fail", fix: "driveTruncated" });
    }
    if (!parseTitleId(path)) problems.push({ id: "gameTitleId", status: "warn", fix: "driveNoId" });
    if (problems[0])
      items.push({
        id: problems[0].id,
        status: problems[0].status,
        title: shown(root, path),
        fix: problems.map((p) => message(language, p.fix)).join("\n"),
      });
  }
  const misplaced: string[] = [];
  for (const e of await entries(root)) {
    if (e.isFile() && /\.(nsp|nsz|xci|xcz)$/i.test(e.name)) misplaced.push(e.name);
    if (e.isDirectory() && e.name !== "games")
      for (const f of await entries(join(root, e.name)))
        if (f.isFile() && /\.(nsp|nsz|xci|xcz)$/i.test(f.name))
          misplaced.push(join(e.name, f.name));
  }
  for (const path of misplaced)
    items.push({
      id: "layout",
      status: "warn",
      title: message(language, "driveLayout", { path }),
      fix: message(language, "driveMove"),
    });
  return { root, items, ready: !items.some((i) => i.status === "fail") };
}

async function copyPlan(root: string, files: LocalFile[], progress: Progress) {
  const pending: LocalFile[] = [];
  const destinations = new Set<string>();
  for (const f of files) {
    safePath(f.name);
    if (destinations.has(f.name.toLowerCase())) throw new PackError("driveConflict");
    destinations.add(f.name.toLowerCase());
    // Refuse symlink ancestors, including the destination itself.
    let current = root;
    for (const segment of f.name.split("/")) {
      current = join(current, segment);
      try {
        const entry = await lstat(current);
        if (
          entry.isSymbolicLink() ||
          (current !== join(root, f.name) && !entry.isDirectory()) ||
          (current === join(root, f.name) && !entry.isFile())
        )
          throw new PackError("driveConflict");
      } catch (e) {
        if ((e as NodeJS.ErrnoException).code !== "ENOENT") throw e;
      }
    }
    const target = join(root, f.name);
    if (await exists(target)) {
      if (Bun.file(target).size !== Bun.file(f.path).size) throw new PackError("driveConflict");
    } else pending.push(f);
  }
  const space = await statfs(root);
  const bytes = pending.reduce((n, f) => n + Bun.file(f.path).size, 0);
  if (space.bavail * space.bsize < bytes) throw new PackError("driveSpace");
  for (const f of pending) {
    const target = join(root, f.name);
    await mkdir(dirname(target), { recursive: true });
    progress(f.name, 0, Bun.file(f.path).size);
    await copyFile(f.path, target, constants.COPYFILE_EXCL);
    progress(f.name, Bun.file(f.path).size, Bun.file(f.path).size);
  }
}
export async function fixDrive(
  root: string,
  report: DriveReport,
  language: Language = report.items[0]?.title === strings.pt.driveProd ? "pt" : "en",
  deps = services,
): Promise<DriveReport> {
  root = resolve(root);
  if (report.root !== root) throw new PackError("inputInvalid");
  if (hasFixableFirmware(report)) {
    const source = await firmwareSource(root),
      keys = await keyPath(root, "prod.keys");
    if (!source.source || !keys) return checkDrive(root, language, deps);
    const temporary = await mkdtemp(join(tmpdir(), "nxhelper-firmware-"));
    try {
      const fw = await deps.prepare(source.source, keys, temporary);
      await copyPlan(
        root,
        fw.files.map((f) => ({ path: f.path, name: `NXbox/firmware/${basename(f.name)}` })),
        () => {},
      );
    } catch (error) {
      if (!(error instanceof PackError)) throw error;
      const updated = await checkDrive(root, language, deps);
      const item = updated.items.find((i) => i.id === "firmware");
      if (item) {
        item.status = "fail";
        item.fix = message(
          language,
          error.code === "firmwareKeys"
            ? "driveOldKeys"
            : ["firmwareIncomplete", "firmwareInvalid", "nandInvalid"].includes(error.code)
              ? "driveFirmwareCopy"
              : error.code,
        );
      }
      updated.ready = false;
      return updated;
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  }
  return checkDrive(root, language, deps);
}
async function validatedInputs(
  inputs: DriveInputs,
  temporary: string,
  deps: DriveServices,
  portal = false,
) {
  if ((await readKeys(inputs.keys)).get("header_key")?.length !== 32)
    throw new PackError("driveKeysFix");
  if (inputs.titleKeys && !validTitleKeys(await readKeys(inputs.titleKeys)))
    throw new PackError("driveTitleInvalid");
  if (!inputs.games.length) throw new PackError("file");
  const games: LocalFile[] = [];
  for (const [index, source] of inputs.games.entries()) {
    await validateFile(source);
    let path = source;
    if (gameExtension(source) === ".xcz") {
      if (!portal) throw new PackError("driveConvert");
      path = await prepareContent(source, join(temporary, `game-${index}`), inputs.keys);
    }
    await validateGame(path);
    games.push({ path, name: safePath(basename(path, extname(path)) + gameExtension(path)) });
  }
  if (new Set(games.map((f) => f.name.toLowerCase())).size !== games.length)
    throw new PackError("driveConflict");
  const firmware = inputs.firmware
    ? await deps.prepare(inputs.firmware, inputs.keys, temporary)
    : undefined;
  return {
    games,
    firmware,
    keys: [
      { path: inputs.keys, name: "prod.keys" },
      ...(inputs.titleKeys ? [{ path: inputs.titleKeys, name: "title.keys" }] : []),
    ],
  };
}
export async function prepareDrive(
  root: string,
  inputs: DriveInputs,
  onProgress: Progress = () => {},
  deps = services,
): Promise<DriveReport> {
  root = resolve(root);
  if (!(await lstat(root)).isDirectory()) throw new PackError("inputInvalid");
  const temporary = await mkdtemp(join(tmpdir(), "nxhelper-drive-"));
  try {
    const plan = await validatedInputs(inputs, temporary, deps);
    await copyPlan(
      root,
      [
        // Keys go in NXbox/, the first place the Xbox looks, so older copies elsewhere never win.
        ...plan.keys.map((k) => ({ ...k, name: `NXbox/${k.name}` })),
        ...(plan.firmware?.files ?? []).map((f) => ({
          path: f.path,
          name: `NXbox/firmware/${basename(f.name)}`,
        })),
        ...plan.games.map((f) => ({ path: f.path, name: `NXbox/games/${f.name}` })),
      ],
      onProgress,
    );
    return await checkDrive(root, inputs.language ?? "en", deps);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
}
export type NxboxPortal = Pick<DevicePortal, "packages" | "makeFolder" | "pushFile"> & {
  freeSpace?: () => Promise<number | undefined>;
};
export async function sendToNxbox(
  portal: NxboxPortal,
  inputs: DriveInputs,
  onProgress: Progress = () => {},
  deps = services,
) {
  const temporary = await mkdtemp(join(tmpdir(), "nxhelper-xbox-"));
  try {
    const plan = await validatedInputs(inputs, temporary, deps, true);
    const packages = (await portal.packages()).filter((p) =>
      p.PackageFullName.startsWith("NSPX.NXbox_"),
    );
    packages.sort((a, b) => {
      const av = a.PackageFullName.split("_")[1]!.split(".").map(Number),
        bv = b.PackageFullName.split("_")[1]!.split(".").map(Number);
      for (let i = 0; i < Math.max(av.length, bv.length); i++) {
        const d = (bv[i] ?? 0) - (av[i] ?? 0);
        if (d) return d;
      }
      return 0;
    });
    if (!packages[0]) throw new PackError("nxboxMissing");
    const pkg = packages[0].PackageFullName;
    const groups = [
      { directory: "LocalState/games", files: plan.games },
      { directory: "LocalState/eden/keys", files: plan.keys },
      { directory: "LocalState/firmware", files: plan.firmware?.files ?? [] },
    ];
    const free = await portal.freeSpace?.();
    if (
      free !== undefined &&
      free < groups.flatMap((g) => g.files).reduce((n, f) => n + Bun.file(f.path).size, 0)
    )
      throw new PackError("driveSpace");
    const warnings =
      free === undefined ? [message(inputs.language ?? "en", "xboxSpaceWarning")] : [];
    // Surface the warning before the first upload, as well as in the final result.
    for (const warning of warnings) onProgress(warning, 0, 0);
    const made = new Set<string>();
    for (const group of groups) {
      if (!group.files.length) continue;
      let parent = "";
      for (const name of group.directory.split("/")) {
        const path = [parent, name].filter(Boolean).join("/");
        if (!made.has(path)) {
          await portal.makeFolder(pkg, parent, name);
          made.add(path);
        }
        parent = path;
      }
      for (const f of group.files)
        await portal.pushFile(pkg, f.path, group.directory, basename(f.name), (sent, total) =>
          onProgress(f.name, sent, total),
        );
    }
    return { package: pkg, warnings };
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
}
