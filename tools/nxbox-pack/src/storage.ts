// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { open, readdir, lstat, mkdir } from "node:fs/promises";
import { createReadStream, createWriteStream } from "node:fs";
import { join, dirname } from "node:path";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { createInflateRaw } from "node:zlib";
import { PackError } from "./i18n";

export interface Reader {
  size: number;
  read(offset: number, length: number): Promise<Buffer>;
}
export function bounded(size: number, offset: number, length: number) {
  if (
    !Number.isSafeInteger(offset) ||
    !Number.isSafeInteger(length) ||
    offset < 0 ||
    length < 0 ||
    offset + length > size
  )
    throw new PackError("inputInvalid");
}
export async function fileReader(paths: string[]): Promise<Reader & { close(): Promise<void> }> {
  const files: { handle: Awaited<ReturnType<typeof open>>; size: number }[] = [];
  try {
    for (const path of paths) {
      const handle = await open(path, "r");
      files.push({ handle, size: (await handle.stat()).size });
    }
  } catch (error) {
    await Promise.all(files.map((f) => f.handle.close()));
    throw error;
  }
  const size = files.reduce((n, f) => n + f.size, 0);
  return {
    size,
    async close() {
      await Promise.all(files.map((f) => f.handle.close()));
    },
    async read(offset, length) {
      bounded(size, offset, length);
      const result = Buffer.alloc(length);
      let copied = 0;
      for (const f of files) {
        if (offset >= f.size) {
          offset -= f.size;
          continue;
        }
        const count = Math.min(length - copied, f.size - offset);
        let got = 0;
        while (got < count) {
          const r = await f.handle.read(result, copied + got, count - got, offset + got);
          if (!r.bytesRead) throw new PackError("inputInvalid");
          got += r.bytesRead;
        }
        copied += count;
        offset = 0;
        if (copied === length) break;
      }
      return result;
    },
  };
}
export function safePath(name: string): string {
  const parts = name.replace(/\/$/, "").split("/");
  if (
    !name ||
    /[\\:\x00-\x1f<>"|?*]/.test(name) ||
    parts.some(
      (p) =>
        !p ||
        p === "." ||
        p === ".." ||
        /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(p) ||
        /[. ]$/.test(p),
    )
  )
    throw new PackError("inputInvalid");
  return name;
}
export type LocalFile = { path: string; name: string };
export async function folderFiles(root: string): Promise<LocalFile[]> {
  const files: LocalFile[] = [];
  async function walk(path: string, prefix: string, depth: number) {
    if (depth > 32 || files.length > 100000) throw new PackError("inputInvalid");
    for (const entry of await readdir(path, { withFileTypes: true })) {
      const name = safePath(prefix + entry.name),
        local = join(path, entry.name);
      if (entry.isSymbolicLink()) throw new PackError("inputInvalid");
      if (entry.isDirectory()) await walk(local, `${name}/`, depth + 1);
      else if (entry.isFile()) files.push({ path: local, name });
      else throw new PackError("inputInvalid");
    }
  }
  if (!(await lstat(root)).isDirectory()) throw new PackError("inputInvalid");
  await walk(root, "", 0);
  if (new Set(files.map((f) => f.name.toLowerCase())).size !== files.length)
    throw new PackError("inputInvalid");
  return files;
}
const crcTable = Uint32Array.from({ length: 256 }, (_, value) => {
  for (let bit = 0; bit < 8; bit++) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
  return value >>> 0;
});
export type ZipEntry = { name: string; size: number; chunks(): AsyncIterable<Buffer> };
// Central-directory parsing avoids holding ZIPs or expanded NAND members in memory.
export async function zipEntries(path: string): Promise<ZipEntry[]> {
  const r = await fileReader([path]);
  try {
    const tail = await r.read(Math.max(0, r.size - 65557), Math.min(r.size, 65557));
    let end = -1;
    for (let i = tail.length - 22; i >= 0; i--)
      if (
        tail.readUInt32LE(i) === 0x06054b50 &&
        i + 22 + tail.readUInt16LE(i + 20) === tail.length
      ) {
        end = i;
        break;
      }
    if (end < 0 || tail.readUInt16LE(end + 4) || tail.readUInt16LE(end + 6))
      throw new PackError("inputInvalid");
    let count = tail.readUInt16LE(end + 10),
      offset = tail.readUInt32LE(end + 16);
    if (count === 65535 || offset === 0xffffffff) {
      const locator = await r.read(r.size - tail.length + end - 20, 20);
      if (locator.readUInt32LE(0) !== 0x07064b50) throw new PackError("inputInvalid");
      const z = await r.read(Number(locator.readBigUInt64LE(8)), 56);
      if (z.readUInt32LE(0) !== 0x06064b50 || z.readUInt32LE(16) || z.readUInt32LE(20))
        throw new PackError("inputInvalid");
      count = Number(z.readBigUInt64LE(32));
      offset = Number(z.readBigUInt64LE(48));
    }
    if (count > 100000) throw new PackError("inputInvalid");
    const entries: ZipEntry[] = [];
    const names = new Set<string>();
    for (let i = 0; i < count; i++) {
      const h = await r.read(offset, 46);
      if (h.readUInt32LE(0) !== 0x02014b50 || h.readUInt16LE(8) & 1)
        throw new PackError("inputInvalid");
      const crcExpected = h.readUInt32LE(16);
      const method = h.readUInt16LE(10),
        nl = h.readUInt16LE(28),
        el = h.readUInt16LE(30);
      const variable = await r.read(offset + 46, nl + el),
        name = safePath(variable.subarray(0, nl).toString("utf8"));
      if (names.has(name.toLowerCase()) || ((h.readUInt32LE(38) >>> 16) & 0xf000) === 0xa000)
        throw new PackError("inputInvalid");
      names.add(name.toLowerCase());
      let size = h.readUInt32LE(24),
        compressed = h.readUInt32LE(20),
        local = h.readUInt32LE(42);
      for (let p = nl; p + 4 <= variable.length; ) {
        const tag = variable.readUInt16LE(p),
          len = variable.readUInt16LE(p + 2);
        p += 4;
        if (p + len > variable.length) throw new PackError("inputInvalid");
        if (tag === 1) {
          let q = p;
          if (size === 0xffffffff) {
            size = Number(variable.readBigUInt64LE(q));
            q += 8;
          }
          if (compressed === 0xffffffff) {
            compressed = Number(variable.readBigUInt64LE(q));
            q += 8;
          }
          if (local === 0xffffffff) local = Number(variable.readBigUInt64LE(q));
        }
        p += len;
      }
      offset += 46 + nl + el + h.readUInt16LE(32);
      if (name.endsWith("/")) continue;
      if (![0, 8].includes(method)) throw new PackError("inputInvalid");
      const lh = await r.read(local, 30);
      if (lh.readUInt32LE(0) !== 0x04034b50) throw new PackError("inputInvalid");
      const start = local + 30 + lh.readUInt16LE(26) + lh.readUInt16LE(28);
      bounded(r.size, start, compressed);
      entries.push({
        name,
        size,
        async *chunks() {
          let total = 0,
            crc = 0xffffffff;
          const source = compressed
            ? createReadStream(path, { start, end: start + compressed - 1 })
            : Readable.from([]);
          const stream = method === 8 ? source.pipe(createInflateRaw()) : source;
          source.on("error", (error) => stream.destroy(error));
          try {
            for await (const chunk of stream) {
              total += chunk.length;
              if (total > size) throw new PackError("inputInvalid");
              for (const byte of chunk) crc = crcTable[(crc ^ byte) & 255]! ^ (crc >>> 8);
              yield Buffer.from(chunk);
            }
            if (total !== size || (crc ^ 0xffffffff) >>> 0 !== crcExpected)
              throw new PackError("inputInvalid");
          } finally {
            source.destroy();
            stream.destroy();
          }
        },
      });
    }
    return entries;
  } finally {
    await r.close();
  }
}
export async function writeChunks(path: string, chunks: AsyncIterable<Uint8Array>) {
  await mkdir(dirname(path), { recursive: true, mode: 0o700 });
  await pipeline(Readable.from(chunks), createWriteStream(path, { mode: 0o600, flags: "wx" }));
}
export async function inputFiles(path: string, staging: string): Promise<LocalFile[]> {
  if ((await lstat(path)).isDirectory()) return folderFiles(path);
  if (!/\.zip$/i.test(path)) throw new PackError("inputInvalid");
  const entries = await zipEntries(path);
  let total = 0;
  const files: LocalFile[] = [];
  for (const e of entries) {
    total += e.size;
    if (total > 16 * 1024 ** 3) throw new PackError("inputInvalid");
    const local = join(staging, e.name);
    await writeChunks(local, e.chunks());
    files.push({ path: local, name: e.name });
  }
  return files;
}
