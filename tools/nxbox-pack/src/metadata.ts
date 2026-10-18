// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// Bounded random-access metadata reads: game content is never extracted to disk.
import { createDecipheriv } from "node:crypto";
import { ncaHeader, readKeys, sectionReader, type Keys } from "./firmware";
import { bounded, fileReader, type Reader } from "./storage";

function slice(r: Reader, offset: number, size: number): Reader {
  bounded(r.size, offset, size);
  return {
    size,
    read: (o, n) => {
      bounded(size, o, n);
      return r.read(offset + o, n);
    },
  };
}
type Entry = { name: string; reader: Reader };
export async function partition(r: Reader, magic: "PFS0" | "HFS0"): Promise<Entry[]> {
  const h = await r.read(0, 16);
  if (h.toString("ascii", 0, 4) !== magic) throw new Error("Invalid partition");
  const count = h.readUInt32LE(4),
    strings = h.readUInt32LE(8),
    stride = magic === "PFS0" ? 24 : 64;
  if (count > 10000 || strings > 4 * 1024 * 1024) throw new Error("Oversized partition");
  const table = await r.read(16, count * stride + strings),
    data = 16 + table.length;
  return Array.from({ length: count }, (_, i) => {
    const o = i * stride,
      nameOffset = table.readUInt32LE(o + 16);
    bounded(strings, nameOffset, 1);
    const start = count * stride + nameOffset,
      end = table.indexOf(0, start);
    if (end < 0) throw new Error("Invalid partition name");
    return {
      name: table.toString("utf8", start, end),
      reader: slice(
        r,
        data + Number(table.readBigUInt64LE(o)),
        Number(table.readBigUInt64LE(o + 8)),
      ),
    };
  });
}
export async function romFiles(section: Reader, fs: Buffer): Promise<Entry[]> {
  let offset = 0;
  if (fs[3] === 3 || fs[3] === 6) {
    const levels = fs.readUInt32LE(0x14);
    if (levels < 2 || levels > 7 || fs.toString("ascii", 8, 12) !== "IVFC")
      throw new Error("Invalid IVFC");
    offset = Number(fs.readBigUInt64LE(0x18 + (levels - 2) * 24));
  } else if (fs[3] === 2 || fs[3] === 5) {
    const levels = fs.readUInt32LE(0x2c);
    if (!levels || levels > 5) throw new Error("Invalid hash layers");
    offset = Number(fs.readBigUInt64LE(0x30 + (levels - 1) * 16));
  } else if (fs[3] !== 1) throw new Error("Unsupported RomFS");
  const rom = slice(section, offset, section.size - offset),
    h = await rom.read(0, 0x50);
  if (h.readBigUInt64LE() !== 0x50n) throw new Error("Invalid RomFS");
  const dirs = Number(h.readBigUInt64LE(0x18)),
    table = Number(h.readBigUInt64LE(0x38)),
    tableSize = Number(h.readBigUInt64LE(0x40)),
    data = Number(h.readBigUInt64LE(0x48));
  const root = await rom.read(dirs, 24),
    files: Entry[] = [],
    seen = new Set<number>();
  let next = root.readUInt32LE(12);
  while (next !== 0xffffffff) {
    if (seen.has(next) || seen.size >= 1000) throw new Error("Cyclic RomFS");
    seen.add(next);
    bounded(tableSize, next, 32);
    const entry = await rom.read(table + next, 32),
      length = entry.readUInt32LE(28);
    if (length > 4096) throw new Error("Oversized RomFS name");
    bounded(tableSize, next + 32, length);
    files.push({
      name: (await rom.read(table + next + 32, length)).toString("utf8"),
      reader: slice(
        rom,
        data + Number(entry.readBigUInt64LE(8)),
        Number(entry.readBigUInt64LE(16)),
      ),
    });
    next = entry.readUInt32LE(4);
  }
  return files;
}
const languages = [
  "AmericanEnglish",
  "BritishEnglish",
  "Japanese",
  "French",
  "German",
  "LatinAmericanSpanish",
  "Spanish",
  "Italian",
  "Dutch",
  "CanadianFrench",
  "Portuguese",
  "Russian",
  "Korean",
  "TraditionalChinese",
  "SimplifiedChinese",
  "BrazilianPortuguese",
];
const text = (b: Buffer, start: number, length: number) =>
  b
    .toString("utf8", start, start + length)
    .replace(/\0.*$/s, "")
    .trim();
export async function controlMetadata(files: Entry[], titleId: string, language = "en") {
  const control = files.find((f) => f.name === "control.nacp");
  if (!control || control.reader.size < 0x3070 || control.reader.size > 0x10000)
    throw new Error("Missing NACP");
  const nacp = await control.reader.read(0, control.reader.size);
  const order = language.toLowerCase().startsWith("pt") ? [15, 10] : [0, 1];
  const selected = [...order, ...languages.map((_, i) => i)].find((i) =>
    text(nacp, i * 0x300, 0x200),
  );
  if (selected === undefined) throw new Error("Empty NACP");
  const icon =
    files.find((f) => f.name === `icon_${languages[selected]}.dat`) ??
    files.find((f) => /^icon_.*\.dat$/.test(f.name));
  let iconBase64: string | undefined;
  if (icon && icon.reader.size <= 1024 * 1024) {
    const bytes = await icon.reader.read(0, icon.reader.size);
    if (bytes[0] === 0xff && bytes[1] === 0xd8 && bytes.at(-2) === 0xff && bytes.at(-1) === 0xd9)
      iconBase64 = bytes.toString("base64");
  }
  return {
    titleId,
    name: text(nacp, selected * 0x300, 0x200),
    publisher: text(nacp, selected * 0x300 + 0x200, 0x100),
    version: text(nacp, 0x3060, 16),
    iconBase64,
  };
}
async function ticketKeys(entries: Entry[], keys: Keys) {
  for (const e of entries.filter((e) => e.name.endsWith(".tik"))) {
    try {
      const type = (await e.reader.read(0, 4)).readUInt32BE();
      const bodyOffset = (
        {
          0x10000: 0x240,
          0x10001: 0x140,
          0x10002: 0x80,
          0x10003: 0x240,
          0x10004: 0x140,
          0x10005: 0x80,
        } as Record<number, number>
      )[type];
      if (bodyOffset === undefined) continue;
      const b = await e.reader.read(bodyOffset, 0x180);
      if (b[0x141] !== 0) continue; // Personalized tickets require the console's private key.
      const generation = Math.max(1, b[0x145]!) - 1,
        kek = keys.get(`titlekek_${generation.toString(16).padStart(2, "0")}`);
      if (kek?.length !== 16) continue;
      const c = createDecipheriv("aes-128-ecb", kek, null);
      c.setAutoPadding(false);
      keys.set(
        `title_${b.subarray(0x160, 0x170).toString("hex")}`,
        Buffer.concat([c.update(b.subarray(0x40, 0x50)), c.final()]),
      );
    } catch {
      /* An unrelated ticket must not prevent control metadata reads. */
    }
  }
}
export async function gameMetadata(path: string, keysPath: string, language: string) {
  const r = await fileReader([path]);
  try {
    const keys = await readKeys(keysPath),
      headerKey = keys.get("header_key");
    if (headerKey?.length !== 32) throw new Error("Missing header key");
    const entries = await packageEntries(r, path);
    await ticketKeys(entries, keys);
    for (const e of entries.filter((e) => e.name.endsWith(".nca"))) {
      try {
        const h = ncaHeader(await e.reader.read(0, 0xc00), headerKey);
        if (h[0x205] !== 2) continue;
        const titleId = (h.readBigUInt64LE(0x210) & ~0xfffn)
          .toString(16)
          .padStart(16, "0")
          .toUpperCase();
        for (let i = 0; i < 4; i++) {
          const fs = h.subarray(0x400 + i * 512, 0x600 + i * 512);
          if (!h.readUInt32LE(0x240 + i * 16) || fs[2] !== 0) continue;
          return await controlMetadata(
            await romFiles(await sectionReader(e.reader, h, i, keys), fs),
            titleId,
            language,
          );
        }
      } catch {
        /* Skip unrelated or unreadable NCAs; inspection retains its filename fallback. */
      }
    }
    throw new Error("Missing readable control NCA");
  } finally {
    await r.close();
  }
}

export async function packageEntries(r: Reader, path: string): Promise<Entry[]> {
  let entries: Entry[];
  if (/\.(nsp|nsz)$/i.test(path)) entries = await partition(r, "PFS0");
  else {
    const card = await r.read(0x100, 0x40);
    if (card.toString("ascii", 0, 4) !== "HEAD") throw new Error("Invalid XCI");
    const offset = Number(card.readBigUInt64LE(0x30));
    const root = await partition(slice(r, offset, r.size - offset), "HFS0");
    const secure = root.find((e) => e.name === "secure");
    if (!secure) throw new Error("Missing secure partition");
    entries = await partition(secure.reader, "HFS0");
  }
  return entries;
}
