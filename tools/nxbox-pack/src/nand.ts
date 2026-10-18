// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { xtsTransform } from "./xts";
import { lstat, readdir } from "node:fs/promises";
import { basename, dirname, join } from "node:path";
import { PackError } from "./i18n";
import { bounded, fileReader, folderFiles, writeChunks, zipEntries, type Reader } from "./storage";

export function xts(data: Buffer, key: Buffer, sector: number | bigint): Buffer {
  const tweak = Buffer.alloc(16);
  tweak.writeBigUInt64LE(BigInt(sector));
  return xtsTransform(data, key, tweak);
}
export function decryptedPartition(
  raw: Reader,
  key: Buffer,
  endian: "little" | "big" = "little",
): Reader {
  return {
    size: raw.size,
    async read(offset, length) {
      bounded(raw.size, offset, length);
      const parts: Buffer[] = [];
      while (length) {
        const sector = Math.floor(offset / 0x4000),
          inside = offset % 0x4000;
        const n = Math.min(length, 0x4000 - inside);
        const encrypted = await raw.read(sector * 0x4000, 0x4000);
        const tweak = Buffer.alloc(16);
        if (endian === "little") tweak.writeBigUInt64LE(BigInt(sector));
        else tweak.writeBigUInt64BE(BigInt(sector), 8);
        const data = xtsTransform(encrypted, key, tweak);
        parts.push(data.subarray(inside, inside + n));
        offset += n;
        length -= n;
      }
      return Buffer.concat(parts);
    },
  };
}
export async function systemPartition(nand: Reader): Promise<{ offset: number; size: number }> {
  const gpt = await nand.read(512, 512);
  if (gpt.toString("ascii", 0, 8) !== "EFI PART" || gpt.readUInt32LE(12) < 92)
    throw new PackError("nandInvalid");
  const offset = Number(gpt.readBigUInt64LE(72)) * 512,
    count = gpt.readUInt32LE(80),
    stride = gpt.readUInt32LE(84);
  if (count > 4096 || stride < 128 || stride > 4096) throw new PackError("nandInvalid");
  const entries = await nand.read(offset, count * stride);
  let found: { offset: number; size: number } | undefined;
  for (let i = 0; i < count; i++) {
    const e = entries.subarray(i * stride, (i + 1) * stride);
    if (e.subarray(56, 128).toString("utf16le").replace(/\0.*$/s, "") !== "SYSTEM") continue;
    if (found) throw new PackError("nandInvalid");
    const first = Number(e.readBigUInt64LE(32)),
      last = Number(e.readBigUInt64LE(40));
    found = { offset: first * 512, size: (last - first + 1) * 512 };
    bounded(nand.size, found.offset, found.size);
    if (!found.size || found.size > 8 * 1024 ** 3 || found.size % 0x4000)
      throw new PackError("nandInvalid");
  }
  if (!found) throw new PackError("nandInvalid");
  return found;
}
export function orderedParts<T extends { name: string }>(files: T[]): T[] {
  const matches = files.filter((f) => /^rawnand\.bin(?:\.\d{2})?$/i.test(basename(f.name)));
  const whole = matches.filter((f) => /^rawnand\.bin$/i.test(basename(f.name)));
  if (whole.length === 1 && matches.length === 1) return whole;
  if (whole.length || !matches.length) throw new PackError("nandInvalid");
  matches.sort((a, b) => a.name.localeCompare(b.name));
  if (matches.some((f, i) => Number(f.name.slice(-2)) !== i)) throw new PackError("nandInvalid");
  return matches;
}
// Each image must come from one directory; never combine different backups.
export function imageParts<T extends { name: string }>(
  files: T[],
  numericRoot = false,
): { parts: T[]; partition: boolean } | undefined {
  const groups = new Map<string, { parts: T[]; partition: boolean }>();
  for (const file of files) {
    const name = basename(file.name),
      parent = dirname(file.name);
    const system = /^SYSTEM(?:\.bin)?$/i.test(name);
    const nand = /^rawnand\.bin(?:\.\d{2})?$/i.test(name);
    const numeric =
      /^\d{2}$/.test(name) && (/^eMMC$/i.test(basename(parent)) || (numericRoot && parent === "."));
    if (!system && !nand && !numeric) continue;
    const id = `${parent}/${system ? name : nand ? "rawnand" : "eMMC"}`.toLowerCase();
    const group = groups.get(id) ?? { parts: [], partition: system };
    group.parts.push(file);
    groups.set(id, group);
  }
  if (!groups.size) return undefined;
  if (groups.size !== 1) throw new PackError("nandInvalid");
  const group = [...groups.values()][0]!;
  if (group.partition) {
    if (group.parts.length !== 1) throw new PackError("nandInvalid");
  } else if (/^rawnand/i.test(basename(group.parts[0]!.name))) {
    group.parts = orderedParts(group.parts);
  } else {
    group.parts.sort((a, b) => a.name.localeCompare(b.name));
    if (group.parts.some((p, i) => Number(basename(p.name)) !== i))
      throw new PackError("nandInvalid");
  }
  return group;
}
async function readSystem<T>(
  raw: Reader,
  key: Buffer | undefined,
  use: (reader: Reader) => Promise<T>,
): Promise<T> {
  // Plain SYSTEM exports need no BIS key. Only retry XTS when the boot sector
  // itself is not FAT32; verification errors from a plain filesystem must survive.
  let plain = false;
  try {
    await Fat32.open(raw);
    plain = true;
  } catch (error) {
    if (!(error instanceof PackError)) throw error;
  }
  if (plain) return use(raw);
  if (!key || key.length !== 32) throw new PackError("firmwareKeys");
  // Sector zero has the same tweak under both conventions. Validate the entire
  // selected NCA set, not just the FAT boot sector, before accepting LE or BE.
  let last: unknown;
  for (const endian of ["little", "big"] as const) {
    try {
      return await use(decryptedPartition(raw, key, endian));
    } catch (error) {
      if (
        !(error instanceof PackError) ||
        !["firmwareInvalid", "firmwareIncomplete", "nandInvalid", "inputInvalid"].includes(
          error.code,
        )
      )
        throw error;
      if (!(last instanceof PackError && last.code === "firmwareIncomplete")) last = error;
    }
  }
  throw last;
}
export async function withSystem<T>(
  path: string,
  temporary: string,
  key: Buffer | undefined,
  use: (reader: Reader) => Promise<T>,
): Promise<T> {
  if (/\.zip$/i.test(path)) {
    const image = imageParts(await zipEntries(path));
    if (!image) throw new PackError("nandInvalid");
    const parts = image.parts,
      size = parts.reduce((n, p) => n + p.size, 0);
    // Read a bounded prefix for GPT, then spool only encrypted SYSTEM bytes.
    let prefix = Buffer.alloc(0);
    for (const p of parts) {
      for await (const chunk of p.chunks()) {
        prefix = Buffer.concat([
          prefix,
          chunk.subarray(0, Math.max(0, 1024 * 1024 - prefix.length)),
        ]);
        if (prefix.length >= Math.min(size, 1024 * 1024)) break;
      }
      if (prefix.length >= Math.min(size, 1024 * 1024)) break;
    }
    const region = image.partition
      ? { offset: 0, size }
      : await systemPartition({
          size,
          async read(offset, length) {
            bounded(prefix.length, offset, length);
            return prefix.subarray(offset, offset + length);
          },
        });
    async function* system() {
      let position = 0;
      for (const p of parts) {
        if (position + p.size <= region.offset) {
          position += p.size;
          continue;
        }
        for await (const chunk of p.chunks()) {
          const from = Math.max(0, region.offset - position),
            to = Math.min(chunk.length, region.offset + region.size - position);
          if (to > from) yield chunk.subarray(from, to);
          position += chunk.length;
          if (position >= region.offset + region.size) return;
        }
      }
    }
    const local = join(temporary, "system.encrypted");
    await writeChunks(local, system());
    const raw = await fileReader([local]);
    try {
      if (raw.size !== region.size) throw new PackError("nandInvalid");
      return await readSystem(raw, key, use);
    } finally {
      await raw.close();
    }
  }
  const directory = (await lstat(path)).isDirectory();
  const files = directory
    ? await folderFiles(path)
    : /^SYSTEM(?:\.bin)?$/i.test(basename(path))
      ? [{ name: basename(path), path }]
      : (await readdir(dirname(path)))
          .filter((name) =>
            /^rawnand\.bin/i.test(basename(path))
              ? /^rawnand\.bin(?:\.\d{2})?$/i.test(name)
              : /^\d{2}$/.test(name),
          )
          .map((name) => ({ name, path: join(dirname(path), name) }));
  const image = imageParts(files, /^eMMC$/i.test(basename(directory ? path : dirname(path))));
  if (!image) throw new PackError("nandInvalid");
  const raw = await fileReader(image.parts.map((p) => p.path));
  try {
    const region = image.partition ? { offset: 0, size: raw.size } : await systemPartition(raw);
    return await readSystem(
      {
        size: region.size,
        read: (offset, length) => {
          bounded(region.size, offset, length);
          return raw.read(region.offset + offset, length);
        },
      },
      key,
      use,
    );
  } finally {
    await raw.close();
  }
}
export type FatEntry = { name: string; cluster: number; size: number; directory: boolean };
export class Fat32 {
  private constructor(
    private readonly r: Reader,
    readonly clusterSize: number,
    private readonly fat: number,
    private readonly data: number,
    private readonly root: number,
    private readonly clusters: number,
    private readonly fatSize: number,
  ) {}
  static async open(r: Reader): Promise<Fat32> {
    const b = await r.read(0, 512),
      sector = b.readUInt16LE(11),
      spc = b[13]!,
      reserved = b.readUInt16LE(14),
      fats = b[16]!,
      fs = b.readUInt32LE(36),
      total = b.readUInt32LE(32);
    if (
      b.readUInt16LE(510) !== 0xaa55 ||
      ![512, 1024, 2048, 4096].includes(sector) ||
      !spc ||
      spc & (spc - 1) ||
      spc > 128 ||
      !reserved ||
      !fats ||
      fats > 2 ||
      !fs ||
      b.readUInt16LE(17) ||
      b.readUInt16LE(22) ||
      total * sector > r.size
    )
      throw new PackError("nandInvalid");
    const data = (reserved + fats * fs) * sector,
      clusters = Math.floor((total * sector - data) / (sector * spc));
    if (clusters <= 0 || (clusters + 2) * 4 > fs * sector) throw new PackError("nandInvalid");
    const flags = b.readUInt16LE(40),
      active = flags & 0x80 ? flags & 15 : 0;
    if (active >= fats) throw new PackError("nandInvalid");
    return new Fat32(
      r,
      sector * spc,
      (reserved + active * fs) * sector,
      data,
      b.readUInt32LE(44),
      clusters,
      fs * sector,
    );
  }
  async *chain(start: number): AsyncGenerator<Buffer> {
    let c = start;
    const seen = new Set<number>();
    while (c < 0x0ffffff8) {
      if (c < 2 || c >= this.clusters + 2 || seen.has(c) || c * 4 >= this.fatSize)
        throw new PackError("nandInvalid");
      seen.add(c);
      yield await this.r.read(this.data + (c - 2) * this.clusterSize, this.clusterSize);
      c = (await this.r.read(this.fat + c * 4, 4)).readUInt32LE() & 0x0fffffff;
    }
  }
  async directory(cluster = this.root): Promise<FatEntry[]> {
    const entries: FatEntry[] = [];
    let long: string[] = [];
    for await (const b of this.chain(cluster))
      for (let p = 0; p < b.length; p += 32) {
        const e = b.subarray(p, p + 32);
        if (e[0] === 0) return entries;
        if (e[0] === 0xe5) {
          long = [];
          continue;
        }
        if (e[11] === 15) {
          const n = e[0]! & 31;
          if (!n || n > 20) throw new PackError("nandInvalid");
          if (e[0]! & 0x40) long = [];
          long[n - 1] = Buffer.concat([e.subarray(1, 11), e.subarray(14, 26), e.subarray(28, 32)])
            .toString("utf16le")
            .replace(/[\0\uffff].*$/s, "");
          continue;
        }
        const short =
          e.subarray(0, 8).toString("ascii").trim() +
          (e.subarray(8, 11).toString("ascii").trim()
            ? `.${e.subarray(8, 11).toString("ascii").trim()}`
            : "");
        const name = long.length ? long.join("") : short;
        long = [];
        if (e[11]! & 8 || name === "." || name === "..") continue;
        if (!name || /[/\\\0]/.test(name)) throw new PackError("nandInvalid");
        entries.push({
          name,
          directory: Boolean(e[11]! & 16),
          cluster: e.readUInt16LE(20) * 65536 + e.readUInt16LE(26),
          size: e.readUInt32LE(28),
        });
        if (entries.length > 100000) throw new PackError("nandInvalid");
      }
    return entries;
  }
  async child(parent: number | undefined, name: string): Promise<FatEntry> {
    const found = (await this.directory(parent)).filter(
      (e) => e.name.toLowerCase() === name.toLowerCase(),
    );
    if (found.length !== 1 || !found[0]!.directory) throw new PackError("nandInvalid");
    return found[0]!;
  }
  async *file(e: FatEntry): AsyncGenerator<Buffer> {
    let left = e.size;
    if (!left || e.directory) throw new PackError("nandInvalid");
    for await (const b of this.chain(e.cluster)) {
      const n = Math.min(left, b.length);
      yield b.subarray(0, n);
      left -= n;
      if (!left) return;
    }
    throw new PackError("nandInvalid");
  }
}
