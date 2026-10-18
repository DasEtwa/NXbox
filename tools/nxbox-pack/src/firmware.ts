// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { xtsTransform } from "./xts";
import { createDecipheriv, createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { mkdir, readFile, rm } from "node:fs/promises";
import { basename, join } from "node:path";
import { PackError } from "./i18n";
import { Fat32, withSystem } from "./nand";
import {
  bounded,
  fileReader,
  inputFiles,
  writeChunks,
  zipEntries,
  type Reader,
  type LocalFile,
} from "./storage";

export type Keys = Map<string, Buffer>;
export async function readKeys(path: string): Promise<Keys> {
  const keys: Keys = new Map();
  for (const line of (await readFile(path, "utf8")).split(/\r?\n/)) {
    const m = line.match(/^\s*([a-z0-9_]+)\s*=\s*([a-f0-9]+)\s*(?:#.*)?$/i);
    if (m && m[2]!.length % 2 === 0) keys.set(m[1]!.toLowerCase(), Buffer.from(m[2]!, "hex"));
  }
  return keys;
}
function key(keys: Keys, name: string, length: number): Buffer {
  const value = keys.get(name);
  if (value?.length !== length) throw new PackError("firmwareKeys");
  return value;
}
export function ncaHeader(data: Buffer, headerKey: Buffer): Buffer {
  const result = Buffer.alloc(data.length);
  for (let offset = 0; offset < data.length; offset += 512) {
    const tweak = Buffer.alloc(16);
    tweak.writeBigUInt64BE(BigInt(offset / 512), 8);
    xtsTransform(data.subarray(offset, offset + 512), headerKey, tweak).copy(result, offset);
  }
  if (result.toString("ascii", 0x200, 0x204) !== "NCA3") throw new PackError("firmwareInvalid");
  return result;
}
function ecb(data: Buffer, key: Buffer) {
  const c = createDecipheriv("aes-128-ecb", key, null);
  c.setAutoPadding(false);
  return Buffer.concat([c.update(data), c.final()]);
}
export async function sectionReader(
  r: Reader,
  h: Buffer,
  index: number,
  keys: Keys,
): Promise<Reader> {
  const fs = h.subarray(0x400 + index * 512, 0x600 + index * 512);
  const start = h.readUInt32LE(0x240 + index * 16) * 512,
    end = h.readUInt32LE(0x244 + index * 16) * 512;
  bounded(r.size, start, end - start);
  let ctr: Buffer | undefined;
  if (fs[4] === 3) {
    const rights = h.subarray(0x230, 0x240);
    const generation = Math.max(1, h[0x206]!, h[0x220]!) - 1;
    const family = ["application", "ocean", "system"][h[0x207]!];
    if (!family) throw new PackError("firmwareInvalid");
    ctr = rights.some(Boolean)
      ? key(keys, `title_${rights.toString("hex")}`, 16)
      : ecb(
          h.subarray(0x320, 0x330),
          key(keys, `key_area_key_${family}_${generation.toString(16).padStart(2, "0")}`, 16),
        );
  } else if (fs[4] !== 1) throw new PackError("firmwareInvalid");
  return {
    size: end - start,
    async read(offset, length) {
      bounded(end - start, offset, length);
      if (!ctr) return r.read(start + offset, length);
      const absolute = start + offset,
        aligned = absolute - (absolute % 16),
        skip = absolute % 16;
      const iv = Buffer.alloc(16);
      iv.writeBigUInt64BE(fs.readBigUInt64LE(0x140));
      iv.writeBigUInt64BE(BigInt(aligned / 16), 8);
      const c = createDecipheriv("aes-128-ctr", ctr, iv);
      return Buffer.concat([c.update(await r.read(aligned, skip + length)), c.final()]).subarray(
        skip,
      );
    },
  };
}
export async function archiveVersion(r: Reader, header: Buffer, keys: Keys): Promise<string> {
  for (let i = 0; i < 4; i++) {
    if (!header.readUInt32LE(0x240 + i * 16)) continue;
    const fs = header.subarray(0x400 + i * 512, 0x600 + i * 512);
    if (fs[2] !== 0) continue;
    const section = await sectionReader(r, header, i, keys);
    let romOffset: number;
    if (fs[3] === 3 || fs[3] === 6) {
      const levels = fs.readUInt32LE(0x14);
      if (levels < 2 || levels > 7 || fs.toString("ascii", 8, 12) !== "IVFC")
        throw new PackError("firmwareInvalid");
      romOffset = Number(fs.readBigUInt64LE(0x18 + (levels - 2) * 24));
    } else if (fs[3] === 2 || fs[3] === 5) {
      const levels = fs.readUInt32LE(0x2c);
      if (!levels || levels > 5) throw new PackError("firmwareInvalid");
      romOffset = Number(fs.readBigUInt64LE(0x30 + (levels - 1) * 16));
    } else if (fs[3] === 1) romOffset = 0;
    else throw new PackError("firmwareInvalid");
    const rom = await section.read(romOffset, 0x50);
    if (rom.readBigUInt64LE() !== 0x50n) throw new PackError("firmwareInvalid");
    const dirs = romOffset + Number(rom.readBigUInt64LE(0x18)),
      fileTable = romOffset + Number(rom.readBigUInt64LE(0x38)),
      data = romOffset + Number(rom.readBigUInt64LE(0x48));
    const root = await section.read(dirs, 24);
    let next = root.readUInt32LE(12);
    const seen = new Set<number>();
    while (next !== 0xffffffff) {
      if (seen.has(next) || seen.size > 1000) throw new PackError("firmwareInvalid");
      seen.add(next);
      const entry = await section.read(fileTable + next, 32),
        nl = entry.readUInt32LE(28);
      if (nl > 4096) throw new PackError("firmwareInvalid");
      const name = (await section.read(fileTable + next + 32, nl)).toString("utf8");
      if (name === "file") {
        if (entry.readBigUInt64LE(16) !== 0x100n) throw new PackError("firmwareInvalid");
        const value = await section.read(data + Number(entry.readBigUInt64LE(8)), 256);
        const version = value.subarray(0x68, 0x80).toString("ascii").replace(/\0.*$/s, "");
        if (!/^\d+\.\d+\.\d+$/.test(version) || version !== `${value[0]}.${value[1]}.${value[2]}`)
          throw new PackError("firmwareInvalid");
        return version;
      }
      next = entry.readUInt32LE(4);
    }
  }
  throw new PackError("firmwareInvalid");
}
export type Firmware = { version: string; files: LocalFile[] };
export async function verifyFirmware(files: LocalFile[], keys: Keys): Promise<Firmware> {
  const verified: LocalFile[] = [];
  const names = new Set<string>();
  let version = "",
    mii = false;
  for (const file of files) {
    const name = basename(file.name).toLowerCase();
    // Dump tools name metadata NCAs <hash>.cnmt.nca; the hash prefix is the same.
    if (!/^[a-f0-9]{32}(?:\.cnmt)?\.nca$/.test(name) || names.has(name))
      throw new PackError("firmwareInvalid");
    names.add(name);
    const hash = createHash("sha256");
    for await (const chunk of createReadStream(file.path)) hash.update(chunk);
    if (hash.digest("hex").slice(0, 32) !== name.slice(0, 32))
      throw new PackError("firmwareInvalid");
    const r = await fileReader([file.path]);
    try {
      const h = ncaHeader(await r.read(0, 0xc00), key(keys, "header_key", 32));
      if (h.readBigUInt64LE(0x208) !== BigInt(r.size)) throw new PackError("firmwareInvalid");
      const title = h.readBigUInt64LE(0x210),
        type = h[0x205];
      if (title === 0x0100000000000809n && type === 4) version = await archiveVersion(r, h, keys);
      if (title === 0x0100000000000802n && (type === 4 || type === 5)) mii = true;
    } finally {
      await r.close();
    }
    verified.push({ path: file.path, name });
  }
  if (!version || !mii) throw new PackError("firmwareIncomplete");
  return { version, files: verified };
}
export async function prepareFirmware(
  path: string,
  keysPath: string,
  temporary: string,
): Promise<Firmware> {
  try {
    await mkdir(temporary, { recursive: true, mode: 0o700 });
    const keys = await readKeys(keysPath);
    let files: LocalFile[];
    const isZip = /\.zip$/i.test(path),
      entries = isZip ? await zipEntries(path) : [];
    const nand =
      /^rawnand\.bin(?:\.\d{2})?$/i.test(basename(path)) ||
      entries.some((e) => /^rawnand\.bin(?:\.\d{2})?$/i.test(basename(e.name)));
    if (nand) {
      return await withSystem(path, temporary, key(keys, "bis_key_02", 32), async (reader) => {
        const extracted = join(temporary, "ncas");
        await rm(extracted, { recursive: true, force: true });
        await mkdir(extracted, { recursive: true, mode: 0o700 });
        const fat = await Fat32.open(reader),
          contents = await fat.child(undefined, "Contents"),
          registered = await fat.child(contents.cluster, "registered");
        const out: LocalFile[] = [],
          seen = new Set<number>();
        async function walk(cluster: number, depth: number) {
          if (depth > 8 || seen.has(cluster)) throw new PackError("nandInvalid");
          seen.add(cluster);
          for (const e of await fat.directory(cluster)) {
            if (/^[a-f0-9]{32}(?:\.cnmt)?\.nca$/i.test(e.name)) {
              const local = join(extracted, e.name.toLowerCase());
              if (e.directory) {
                const chunks = await fat.directory(e.cluster);
                chunks.sort((a, b) => a.name.localeCompare(b.name));
                if (
                  !chunks.length ||
                  chunks.some(
                    (c, i) => c.directory || !/^\d{2}$/.test(c.name) || Number(c.name) !== i,
                  )
                )
                  throw new PackError("nandInvalid");
                async function* concatenate() {
                  for (const chunk of chunks) yield* fat.file(chunk);
                }
                await writeChunks(local, concatenate());
              } else await writeChunks(local, fat.file(e));
              out.push({ path: local, name: e.name });
            } else if (e.directory) await walk(e.cluster, depth + 1);
          }
        }
        await walk(registered.cluster, 0);
        return await verifyFirmware(out, keys);
      });
    } else {
      files = (await inputFiles(path, join(temporary, "ncas"))).filter((f) =>
        /\.nca$/i.test(f.name),
      );
    }
    return await verifyFirmware(files, keys);
  } catch (cause) {
    if (
      cause instanceof PackError &&
      ["firmwareKeys", "firmwareIncomplete", "nandInvalid"].includes(cause.code)
    )
      throw cause;
    throw new PackError("firmwareInvalid", { cause });
  }
}
