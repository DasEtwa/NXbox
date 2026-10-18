// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { createCipheriv } from "node:crypto";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { gameDestination } from "../src/files";
import { inspect } from "../src/api";
import { controlMetadata, partition, romFiles } from "../src/metadata";
import { xtsTransform } from "../src/xts";

const headerKey = Buffer.from(Array.from({ length: 32 }, (_, i) => i + 7));
const area = Buffer.alloc(16, 0x35),
  sectionKey = Buffer.alloc(16, 0x57);
const jpeg = readFileSync(new URL("./fixtures/synthetic-icon.jpg", import.meta.url));
const reader = (data: Buffer) => ({
  size: data.length,
  async read(o: number, n: number) {
    return data.subarray(o, o + n);
  },
});
function nacp() {
  const b = Buffer.alloc(0x4000);
  b.write("Official English title");
  b.write("English publisher", 0x200);
  b.write("Título brasileiro", 15 * 0x300);
  b.write("Editora", 15 * 0x300 + 0x200);
  b.write("2.4.1", 0x3060);
  return b;
}
function rom() {
  const b = Buffer.alloc(0x5000);
  b.writeBigUInt64LE(0x50n);
  b.writeBigUInt64LE(0x50n, 0x18);
  b.writeBigUInt64LE(0x80n, 0x38);
  b.writeBigUInt64LE(0x100n, 0x40);
  b.writeBigUInt64LE(0x200n, 0x48);
  b.writeUInt32LE(0, 0x5c);
  const files = [
    { name: "control.nacp", data: nacp() },
    { name: "icon_BrazilianPortuguese.dat", data: jpeg },
    { name: "icon_AmericanEnglish.dat", data: jpeg },
  ];
  let next = 0,
    offset = 0;
  for (const [i, f] of files.entries()) {
    const name = Buffer.from(f.name),
      pos = 0x80 + next,
      after = next + 32 + name.length;
    b.writeUInt32LE(i === files.length - 1 ? 0xffffffff : after, pos + 4);
    b.writeBigUInt64LE(BigInt(offset), pos + 8);
    b.writeBigUInt64LE(BigInt(f.data.length), pos + 16);
    b.writeUInt32LE(name.length, pos + 28);
    name.copy(b, pos + 32);
    f.data.copy(b, 0x200 + offset);
    next = after;
    offset += f.data.length;
  }
  return b;
}
function nca(encrypted: boolean, rights = false, titleId = 0x0100123456780000n, contentType = 2) {
  const section = Buffer.concat([Buffer.alloc(0x200), rom()]),
    size = Math.ceil((0xc00 + section.length) / 512) * 512,
    data = Buffer.alloc(size),
    h = Buffer.alloc(0xc00);
  h.write("NCA3", 0x200);
  h[0x205] = contentType;
  h.writeBigUInt64LE(BigInt(size), 0x208);
  h.writeBigUInt64LE(titleId, 0x210);
  h.writeUInt32LE(6, 0x240);
  h.writeUInt32LE(size / 512, 0x244);
  const fs = h.subarray(0x400, 0x600);
  fs[3] = 3;
  fs[4] = encrypted ? 3 : 1;
  fs.write("IVFC", 8);
  fs.writeUInt32LE(7, 0x14);
  fs.writeBigUInt64LE(0x200n, 0x90);
  section.copy(data, 0xc00);
  if (encrypted) {
    if (rights) h.fill(0x11, 0x230, 0x240);
    const ecb = createCipheriv("aes-128-ecb", area, null);
    ecb.setAutoPadding(false);
    ecb.update(sectionKey).copy(h, 0x320);
    ecb.final();
    fs.writeBigUInt64LE(0x11223344n, 0x140);
    const iv = Buffer.alloc(16);
    iv.writeBigUInt64BE(0x11223344n);
    iv.writeBigUInt64BE(0xc0n, 8);
    const ctr = createCipheriv("aes-128-ctr", sectionKey, iv);
    Buffer.concat([ctr.update(data.subarray(0xc00)), ctr.final()]).copy(data, 0xc00);
  }
  for (let i = 0; i < 6; i++) {
    const iv = Buffer.alloc(16);
    iv.writeBigUInt64BE(BigInt(i), 8);
    xtsTransform(h.subarray(i * 512, (i + 1) * 512), headerKey, iv, true).copy(data, i * 512);
  }
  return data;
}
function pack(files: { name: string; data: Buffer }[], hfs = false) {
  const stride = hfs ? 64 : 24,
    names = Buffer.from(files.map((f) => f.name + "\0").join("")),
    header = Buffer.alloc(16 + stride * files.length + names.length);
  header.write(hfs ? "HFS0" : "PFS0");
  header.writeUInt32LE(files.length, 4);
  header.writeUInt32LE(names.length, 8);
  names.copy(header, 16 + stride * files.length);
  let offset = 0,
    nameOffset = 0;
  for (const [i, f] of files.entries()) {
    const pos = 16 + i * stride;
    header.writeBigUInt64LE(BigInt(offset), pos);
    header.writeBigUInt64LE(BigInt(f.data.length), pos + 8);
    header.writeUInt32LE(nameOffset, pos + 16);
    offset += f.data.length;
    nameOffset += Buffer.byteLength(f.name) + 1;
  }
  return Buffer.concat([header, ...files.map((f) => f.data)]);
}
async function workspace(run: (root: string, keys: string) => Promise<void>) {
  const root = await mkdtemp(join(tmpdir(), "nxbox-metadata-test-")),
    keys = join(root, "prod.keys");
  try {
    await writeFile(
      keys,
      `header_key = ${headerKey.toString("hex")}\nkey_area_key_application_00 = ${area.toString("hex")}\ntitlekek_00 = ${area.toString("hex")}\n`,
    );
    await run(root, keys);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}
test("localized NACP fields, JPEG and first available language fallback", async () => {
  const files = [
    { name: "control.nacp", reader: reader(nacp()) },
    { name: "icon_BrazilianPortuguese.dat", reader: reader(jpeg) },
  ];
  expect(await controlMetadata(files, "id", "pt-BR")).toEqual({
    titleId: "id",
    name: "Título brasileiro",
    publisher: "Editora",
    version: "2.4.1",
    iconBase64: jpeg.toString("base64"),
  });
  expect((await controlMetadata(files, "id", "en")).name).toBe("Official English title");
  const only = Buffer.alloc(0x4000);
  only.write("日本語", 2 * 0x300);
  expect(
    (await controlMetadata([{ name: "control.nacp", reader: reader(only) }], "id", "pt")).name,
  ).toBe("日本語");
});
test("NSP encrypted control NCA replaces misleading filename and reads AES-CTR", async () =>
  workspace(async (root, keys) => {
    for (const encrypted of [false, true]) {
      const path = join(root, "Wrong name [0100000000000000].nsp");
      await writeFile(path, pack([{ name: "control.nca", data: nca(encrypted) }]));
      const info = await inspect(path, keys, "pt");
      expect(info.name).toBe("Título brasileiro");
      expect(info.titleId).toBe("0100123456780000");
      expect(info.version).toBe("2.4.1");
      expect(info.publisher).toBe("Editora");
      expect(info.iconBase64).toBe(jpeg.toString("base64"));
    }
  }));
test("XCI nested secure HFS0 resolves control NCA", async () =>
  workspace(async (root, keys) => {
    const secure = pack([{ name: "control.nca", data: nca(true) }], true),
      rootPartition = pack([{ name: "secure", data: secure }], true),
      prefix = Buffer.alloc(0x10000);
    prefix.write("HEAD", 0x100);
    prefix.writeBigUInt64LE(0x10000n, 0x130);
    const path = join(root, "Example.xci");
    await writeFile(path, Buffer.concat([prefix, rootPartition]));
    expect((await inspect(path, keys)).name).toBe("Official English title");
  }));
test("common ticket supplies rights-managed control title key", async () =>
  workspace(async (root, keys) => {
    const ticket = Buffer.alloc(0x2c0);
    ticket.writeUInt32BE(0x10004);
    const ecb = createCipheriv("aes-128-ecb", area, null);
    ecb.setAutoPadding(false);
    ecb.update(sectionKey).copy(ticket, 0x180);
    ecb.final();
    ticket.fill(0x11, 0x2a0, 0x2b0);
    const path = join(root, "Example.nsp");
    await writeFile(
      path,
      pack([
        { name: "control.nca", data: nca(true, true) },
        { name: "title.tik", data: ticket },
      ]),
    );
    expect((await inspect(path, keys)).version).toBe("2.4.1");
  }));
test("missing keys, malformed container, wrong keys and compressed inputs retain fallback", async () =>
  workspace(async (root, keys) => {
    for (const ext of ["nsp", "xci", "nsz", "xcz"]) {
      const path = join(root, `Fallback [0100123456780000].${ext}`);
      await writeFile(path, Buffer.from("invalid"));
      expect((await inspect(path, keys)).name).toBe("Fallback");
      expect((await inspect(path)).titleId).toBe("0100123456780000");
    }
    const path = join(root, "Fallback.nsp");
    await writeFile(path, pack([{ name: "control.nca", data: nca(true) }]));
    await writeFile(keys, "header_key = 00\n");
    expect((await inspect(path, keys)).name).toBe("Fallback");
  }));
test("partition bounds and cyclic RomFS entries are rejected", async () => {
  const bad = pack([{ name: "control.nca", data: Buffer.alloc(1) }]);
  bad.writeBigUInt64LE(999999n, 16);
  await expect(partition(reader(bad), "PFS0")).rejects.toThrow();
  const cyclic = rom();
  cyclic.writeUInt32LE(0, 0x84);
  const fs = Buffer.alloc(512);
  fs[3] = 1;
  await expect(romFiles(reader(cyclic), fs)).rejects.toThrow("Cyclic");
});

for (const [id, folder] of [
  [0x0100123456780000n, "Base"],
  [0x0100123456780800n, "Updates"],
  [0x0100123456781001n, "DLC"],
] as const) {
  test(`encrypted Meta NCA routes ${folder} without filename ID in NSP, NSZ and XCI`, async () =>
    workspace(async (root, keys) => {
      const meta = [{ name: "meta.cnmt.nca", data: nca(false, false, id, 1) }];
      for (const extension of ["nsp", "nsz", "xci"]) {
        const path = join(root, `No ID.${extension}`);
        let bytes = pack(meta);
        if (extension === "xci") {
          const prefix = Buffer.alloc(0x10000);
          prefix.write("HEAD", 0x100);
          prefix.writeBigUInt64LE(0x10000n, 0x130);
          bytes = Buffer.concat([prefix, pack([{ name: "secure", data: pack(meta, true) }], true)]);
        }
        await writeFile(path, bytes);
        expect(await gameDestination(path, keys)).toBe(`${folder}/No ID.${extension}`);
      }
      const misleading = join(root, "Wrong [0100123456780000].nsp");
      await writeFile(misleading, pack(meta));
      expect(await gameDestination(misleading, keys)).toBe(
        `${folder}/Wrong [0100123456780000].nsp`,
      );
    }));
}
