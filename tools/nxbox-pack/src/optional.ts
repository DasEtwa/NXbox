import { basename, extname, join } from "node:path";
import { randomBytes } from "node:crypto";
import { mkdir, writeFile, lstat } from "node:fs/promises";
import { PackError } from "./i18n";
import { inputFiles, safePath, type LocalFile } from "./storage";

export type Transfer = { path: string; destination: string };
export type OptionalContent = {
  firmware?: string;
  saves?: string;
  mods?: string;
  titleKeys?: string;
};
function checkTitle(files: LocalFile[], titleId: string, source: string) {
  for (const name of [source, ...files.map((f) => f.name)]) {
    const ids = name.match(/0100[0-9a-f]{12}/gi) ?? [];
    if (ids.some((id) => id.toUpperCase() !== titleId)) throw new PackError("optionalMismatch");
  }
}
export async function prepareSaves(
  path: string,
  titleId: string,
  temporary: string,
): Promise<{ files: Transfer[]; count: number }> {
  const files = await inputFiles(path, temporary);
  checkTitle(files, titleId, path);
  if (!files.length) throw new PackError("inputInvalid");
  // A backup leaf may be dropped directly. A ZIP may contain exactly one enclosing backup directory.
  let names = files.map((f) => f.name);
  if (
    /\.zip$/i.test(path) &&
    names.every((n) => n.includes("/")) &&
    new Set(names.map((n) => n.split("/")[0])).size === 1
  )
    names = names.map((n) => n.slice(n.indexOf("/") + 1));
  await mkdir(temporary, { recursive: true, mode: 0o700 });
  const uuid = randomBytes(16),
    profile = Buffer.alloc(0x650);
  uuid.copy(profile, 0x10);
  uuid.copy(profile, 0x20);
  profile.write("Player", 0x38, "utf8");
  const profilePath = join(temporary, "pack-profile.dat");
  await writeFile(profilePath, profile, { mode: 0o600 });
  const user =
    uuid.readBigUInt64LE(8).toString(16).padStart(16, "0") +
    uuid.readBigUInt64LE(0).toString(16).padStart(16, "0");
  const root = `LocalState/eden/nand/user/save/0000000000000000/${user.toUpperCase()}/${titleId}`;
  return {
    count: files.length,
    files: [
      {
        path: profilePath,
        destination: "LocalState/eden/nand/system/save/8000000000000010/su/avators/profiles.dat",
      },
      ...files.map((f, i) => ({ path: f.path, destination: `${root}/${safePath(names[i]!)}` })),
    ],
  };
}
export async function prepareMods(
  path: string,
  titleId: string,
  temporary: string,
): Promise<{ files: Transfer[]; count: number }> {
  const files = await inputFiles(path, temporary);
  checkTitle(files, titleId, path);
  if (!files.length) throw new PackError("inputInvalid");
  const fallback = basename(path, extname(path));
  const names = new Set<string>();
  const transfers = files.map((f) => {
    const parts = f.name.split("/"),
      idx = parts.findIndex((p) => /^(romfs|exefs)$/i.test(p));
    let name: string;
    if (idx >= 0) {
      const mod =
        idx > 0 && !/^0100[0-9a-f]{12}$/i.test(parts[idx - 1]!)
          ? parts[idx - 1]!
          : /^(romfs|exefs)$/i.test(fallback)
            ? "Imported"
            : fallback;
      name = `${mod}/${parts[idx]!.toLowerCase()}/${parts.slice(idx + 1).join("/")}`;
      names.add(mod);
    } else if (/^(romfs|exefs)$/i.test(basename(path))) {
      name = `Imported/${basename(path).toLowerCase()}/${f.name}`;
      names.add("Imported");
    } else throw new PackError("modsInvalid");
    safePath(name);
    return { path: f.path, destination: `LocalState/eden/load/${titleId}/${name}` };
  });
  if (new Set(transfers.map((f) => f.destination.toLowerCase())).size !== transfers.length)
    throw new PackError("modsInvalid");
  return { files: transfers, count: transfers.length };
}

export async function prepareTitleKeys(path: string): Promise<Transfer> {
  try {
    if (
      basename(path).toLowerCase() !== "title.keys" ||
      !(await lstat(path)).isFile() ||
      !Bun.file(path).size
    )
      throw new Error("Invalid title key file");
    return { path, destination: "LocalState/eden/keys/title.keys" };
  } catch (cause) {
    throw new PackError("titleKeysInvalid", { cause });
  }
}
