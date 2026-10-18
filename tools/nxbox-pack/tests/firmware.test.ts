// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { expect, test } from "bun:test";
import { createCipheriv, createHash } from "node:crypto";
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { deflateRawSync } from "node:zlib";
import { checkDrive, fixDrive, hasFixableFirmware, prepareDrive, sendToNxbox } from "../src/drive";
import { strings } from "../src/i18n";
import { zipSync } from "fflate";
import { prepareFirmware, ncaHeader, verifyFirmware } from "../src/firmware";
import { Fat32, decryptedPartition, orderedParts, systemPartition, xts } from "../src/nand";
import { xtsTransform } from "../src/xts";
import { fileReader, inputFiles } from "../src/storage";

const headerKey = Buffer.from(Array.from({ length: 32 }, (_, i) => i + 1));
const bis = Buffer.from(Array.from({ length: 32 }, (_, i) => i));
const area = Buffer.alloc(16, 0x33),
  sectionKey = Buffer.alloc(16, 0x55);
function cipher(data: Buffer, key: Buffer, tweak: Buffer, algorithm = "aes-128-xts") {
  if (algorithm === "aes-128-xts") return xtsTransform(data, key, tweak, true);
  const c = createCipheriv(algorithm, key, tweak);
  c.setAutoPadding(false);
  return Buffer.concat([c.update(data), c.final()]);
}
export function nca(
  title: bigint,
  contentType = 4,
  encryptedSection = false,
): { name: string; data: Buffer } {
  const data = Buffer.alloc(0x2000),
    h = Buffer.alloc(0xc00);
  h.write("NCA3", 0x200);
  h[0x205] = contentType;
  h.writeBigUInt64LE(BigInt(data.length), 0x208);
  h.writeBigUInt64LE(title, 0x210);
  h.writeUInt32LE(6, 0x240);
  h.writeUInt32LE(16, 0x244);
  const fs = h.subarray(0x400, 0x600);
  fs[2] = 0;
  fs[3] = 3;
  fs[4] = encryptedSection ? 3 : 1;
  fs.write("IVFC", 8);
  fs.writeUInt32LE(7, 0x14);
  fs.writeBigUInt64LE(0x200n, 0x90);
  const rom = data.subarray(0xe00);
  rom.writeBigUInt64LE(0x50n);
  rom.writeBigUInt64LE(0x50n, 0x18);
  rom.writeBigUInt64LE(24n, 0x20);
  rom.writeBigUInt64LE(0x80n, 0x38);
  rom.writeBigUInt64LE(36n, 0x40);
  rom.writeBigUInt64LE(0x100n, 0x48);
  rom.writeUInt32LE(0, 0x50 + 12);
  rom.writeUInt32LE(0xffffffff, 0x80 + 4);
  rom.writeBigUInt64LE(256n, 0x80 + 16);
  rom.writeUInt32LE(4, 0x80 + 28);
  rom.write("file", 0xa0);
  rom.set([18, 1, 0], 0x100);
  rom.write("18.1.0", 0x168);
  if (encryptedSection) {
    const c = createCipheriv("aes-128-ecb", area, null);
    c.setAutoPadding(false);
    c.update(sectionKey).copy(h, 0x320);
    c.final();
    fs.writeBigUInt64LE(0x0102030405060708n, 0x140);
    const iv = Buffer.alloc(16);
    iv.writeBigUInt64BE(0x0102030405060708n);
    iv.writeBigUInt64BE(0xc0n, 8);
    cipher(data.subarray(0xc00), sectionKey, iv, "aes-128-ctr").copy(data, 0xc00);
  }
  for (let i = 0; i < 6; i++) {
    const tweak = Buffer.alloc(16);
    tweak.writeBigUInt64BE(BigInt(i), 8);
    cipher(h.subarray(i * 512, (i + 1) * 512), headerKey, tweak).copy(data, i * 512);
  }
  return { data, name: createHash("sha256").update(data).digest("hex").slice(0, 32) + ".nca" };
}
const version = nca(0x0100000000000809n, 4, true),
  mii = nca(0x0100000000000802n, 5);
function fatImage(): Buffer {
  const image = Buffer.alloc(0x10000),
    fat = image.subarray(512, 1024);
  let next = 2;
  image.writeUInt16LE(512, 11);
  image[13] = 1;
  image.writeUInt16LE(1, 14);
  image[16] = 1;
  image.writeUInt32LE(128, 32);
  image.writeUInt32LE(1, 36);
  image.writeUInt32LE(2, 44);
  image.writeUInt16LE(0xaa55, 510);
  function allocate(bytes: Buffer): number {
    const start = next,
      count = Math.max(1, Math.ceil(bytes.length / 512));
    for (let i = 0; i < count; i++) {
      const c = next++;
      fat.writeUInt32LE(i + 1 === count ? 0x0fffffff : c + 1, c * 4);
      bytes.subarray(i * 512, (i + 1) * 512).copy(image, 1024 + (c - 2) * 512);
    }
    return start;
  }
  function entry(name: string, cluster: number, size: number, dir = false): Buffer {
    const out = Buffer.alloc(32);
    const [base, ext = ""] = name.split(".");
    out.write(base!.padEnd(8), 0, "ascii");
    out.write(ext.padEnd(3), 8, "ascii");
    out[11] = dir ? 16 : 32;
    out.writeUInt16LE(cluster, 26);
    out.writeUInt32LE(size, 28);
    return out;
  }
  function longEntry(name: string, cluster: number, size: number, dir = false): Buffer {
    const count = Math.ceil((name.length + 1) / 13),
      out: Buffer[] = [];
    for (let i = count; i >= 1; i--) {
      const e = Buffer.alloc(32, 0xff);
      e[0] = i | (i === count ? 0x40 : 0);
      e[11] = 15;
      e[12] = 0;
      e[13] = 0;
      e.writeUInt16LE(0, 26);
      const text = Buffer.from(
        (name + "\0").slice((i - 1) * 13, i * 13).padEnd(13, "\uffff"),
        "utf16le",
      );
      text.subarray(0, 10).copy(e, 1);
      text.subarray(10, 22).copy(e, 14);
      text.subarray(22).copy(e, 28);
      out.push(e);
    }
    out.push(entry("ARCHIV~1.NCA", cluster, size, dir));
    return Buffer.concat(out);
  }
  const root = allocate(Buffer.alloc(512)),
    contents = allocate(Buffer.alloc(512)),
    registered = allocate(Buffer.alloc(512));
  entry("CONTENTS", contents, 0, true).copy(image, 1024 + (root - 2) * 512);
  longEntry("registered", registered, 0, true).copy(image, 1024 + (contents - 2) * 512);
  const chunk1 = allocate(version.data.subarray(0, 4096)),
    chunk2 = allocate(version.data.subarray(4096));
  const chunks = allocate(Buffer.concat([entry("00", chunk1, 4096), entry("01", chunk2, 4096)]));
  const model = allocate(mii.data);
  Buffer.concat([
    longEntry(version.name, chunks, 0, true),
    longEntry(mii.name, model, mii.data.length),
  ]).copy(image, 1024 + (registered - 2) * 512);
  return image;
}
function nandImage(bigEndian = false): Buffer {
  const system = fatImage(),
    data = Buffer.alloc(0x8000 + system.length + 0x4000);
  data.write("EFI PART", 512);
  data.writeUInt32LE(92, 524);
  data.writeBigUInt64LE(2n, 584);
  data.writeUInt32LE(1, 592);
  data.writeUInt32LE(128, 596);
  data.writeBigUInt64LE(64n, 1056);
  data.writeBigUInt64LE(191n, 1064);
  data.write("SYSTEM", 1080, "utf16le");
  for (let i = 0; i < system.length / 0x4000; i++) {
    const tweak = Buffer.alloc(16);
    if (bigEndian) tweak.writeBigUInt64BE(BigInt(i), 8);
    else tweak.writeBigUInt64LE(BigInt(i));
    cipher(system.subarray(i * 0x4000, (i + 1) * 0x4000), bis, tweak).copy(
      data,
      0x8000 + i * 0x4000,
    );
  }
  return data;
}
async function workspace(run: (root: string, keys: string) => Promise<void>) {
  const root = await mkdtemp(join(tmpdir(), "nxbox-pack-firmware-test-"));
  try {
    const keys = join(root, "prod.keys");
    await writeFile(
      keys,
      `header_key = ${headerKey.toString("hex")}\nbis_key_02 = ${bis.toString("hex")}\nkey_area_key_application_00 = ${area.toString("hex")}\n`,
    );
    await run(root, keys);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}
test("AES-XTS fixed nonzero little-endian sector vector (Python cryptography reference)", () => {
  const encrypted = Buffer.from(
    "815561ed2921dcb716686ad1992e363800d0dbc6a31a88d024a3f4bace68bb91",
    "hex",
  );
  expect(xts(encrypted, bis, 0x0102030405)).toEqual(
    Buffer.from(Array.from({ length: 32 }, (_, i) => i)),
  );
});
test("AES-XTS full 0x4000 sector agrees with independent reference", () => {
  const plain = Buffer.from(Array.from({ length: 0x4000 }, (_, i) => i % 256)),
    tweak = Buffer.alloc(16);
  tweak.writeBigUInt64LE(257n);
  const encrypted = xtsTransform(plain, bis, tweak, true);
  expect(createHash("sha256").update(encrypted).digest("hex")).toBe(
    "750ed71fe08263bd7a1aeb71f4c15a71c7f95a498df50d93ecfaec2b7ec31c00",
  );
  expect(xts(encrypted, bis, 257)).toEqual(plain);
});
test("NCA3 header uses Nintendo big-endian sector tweaks", () => {
  expect(ncaHeader(version.data.subarray(0, 0xc00), headerKey).readBigUInt64LE(0x210)).toBe(
    0x0100000000000809n,
  );
  expect(() => ncaHeader(version.data.subarray(0, 0xc00), bis)).toThrow("firmwareInvalid");
});
test("firmware folder and NCA ZIP identify encrypted SystemVersion and require Mii", async () =>
  workspace(async (root, keys) => {
    const folder = join(root, "firmware");
    await mkdir(folder);
    for (const file of [version, mii]) await writeFile(join(folder, file.name), file.data);
    expect((await prepareFirmware(folder, keys, join(root, "out"))).version).toBe("18.1.0");
    const zip = join(root, "firmware.zip");
    await writeFile(
      zip,
      zipSync({ [`firmware/${version.name}`]: version.data, [`firmware/${mii.name}`]: mii.data }),
    );
    expect((await prepareFirmware(zip, keys, join(root, "zip-out"))).files).toHaveLength(2);
    await rm(join(folder, mii.name));
    await expect(prepareFirmware(folder, keys, join(root, "incomplete"))).rejects.toThrow(
      "firmwareIncomplete",
    );
    await writeFile(join(folder, version.name), Buffer.alloc(version.data.length));
    await expect(prepareFirmware(folder, keys, join(root, "corrupt"))).rejects.toThrow(
      "firmwareInvalid",
    );
  }));
test("GPT SYSTEM only, FAT32 long names and chunked NCA directories across split NAND and ZIP", async () =>
  workspace(async (root, keys) => {
    const nand = nandImage();
    const path = join(root, "rawnand.bin.00");
    await writeFile(path, nand.subarray(0, 0xa123));
    await writeFile(join(root, "rawnand.bin.01"), nand.subarray(0xa123));
    const result = await prepareFirmware(path, keys, join(root, "nand-out"));
    expect(result.version).toBe("18.1.0");
    expect(result.files.map((f) => f.name).sort()).toEqual([version.name, mii.name].sort());
    expect(
      (await readFile(result.files.find((f) => f.name === version.name)!.path)).equals(
        version.data,
      ),
    ).toBe(true);
    const zip = join(root, "nand.zip");
    await writeFile(
      zip,
      zipSync({
        "backup/rawnand.bin.00": nand.subarray(0, 0xa123),
        "backup/rawnand.bin.01": nand.subarray(0xa123),
        "backup/BOOT0": Buffer.alloc(500),
      }),
    );
    expect((await prepareFirmware(zip, keys, join(root, "nand-zip-out"))).version).toBe("18.1.0");
    expect((await readdir(join(root, "nand-zip-out"))).sort()).toEqual(
      ["ncas", "system.encrypted"].sort(),
    );
    expect((await readFile(join(root, "nand-zip-out/system.encrypted"))).length).toBe(0x10000);
    await writeFile(
      keys,
      `header_key = ${headerKey.toString("hex")}\nbis_key_02 = ${headerKey.toString("hex")}`,
    );
    await expect(prepareFirmware(path, keys, join(root, "wrong-bis"))).rejects.toThrow(
      "nandInvalid",
    );
  }));
test("unsplit Nintendo BE NAND convention is detected without changing LE vector semantics", async () =>
  workspace(async (root, keys) => {
    const path = join(root, "rawnand.bin");
    await writeFile(path, nandImage(true));
    expect((await prepareFirmware(path, keys, join(root, "out"))).version).toBe("18.1.0");
  }));
test("rejects missing splits, ambiguous NAND and ZIP traversal", async () =>
  workspace(async (root) => {
    expect(() => orderedParts([{ name: "rawnand.bin.00" }, { name: "rawnand.bin.02" }])).toThrow();
    expect(() => orderedParts([{ name: "rawnand.bin" }, { name: "rawnand.bin.00" }])).toThrow();
    const zip = join(root, "bad.zip");
    await writeFile(zip, zipSync({ "../escape": Buffer.from("x") }));
    await expect(inputFiles(zip, join(root, "extract"))).rejects.toThrow("inputInvalid");
  }));
test("FAT32 cyclic chains fail and decrypted reads span BIS sectors", async () => {
  const plain = fatImage();
  const reader = {
    size: plain.length,
    async read(offset: number, length: number) {
      return plain.subarray(offset, offset + length);
    },
  };
  const fat = await Fat32.open(reader);
  plain.writeUInt32LE(2, 512 + 8);
  // Fill the root directory to force following the cycle.
  plain.fill(0xe5, 1024, 1536);
  await expect(fat.directory()).rejects.toThrow("nandInvalid");
  const data = Buffer.alloc(0x8000, 0x42),
    encrypted = Buffer.alloc(data.length);
  for (let i = 0; i < 2; i++) {
    const tweak = Buffer.alloc(16);
    tweak.writeBigUInt64LE(BigInt(i));
    cipher(data.subarray(i * 0x4000, (i + 1) * 0x4000), bis, tweak).copy(encrypted, i * 0x4000);
  }
  expect(
    await decryptedPartition(
      {
        size: encrypted.length,
        async read(o, n) {
          return encrypted.subarray(o, o + n);
        },
      },
      bis,
    ).read(0x3ff0, 64),
  ).toEqual(data.subarray(0x3ff0, 0x4030));
});

// Small ZIP64 archive with ZIP64 sizes, offsets, EOCD and locator, built without
// real firmware. Force DEFLATE so a stored-only implementation cannot pass.
function zip64(files: Record<string, Buffer>): Buffer {
  const locals: Buffer[] = [],
    central: Buffer[] = [];
  let offset = 0;
  for (const [name, data] of Object.entries(files)) {
    const filename = Buffer.from(name),
      compressed = deflateRawSync(data);
    let crc = 0xffffffff;
    for (const byte of data) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
    }
    crc = (crc ^ 0xffffffff) >>> 0;
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50);
    local.writeUInt16LE(45, 4);
    local.writeUInt16LE(8, 8);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(compressed.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(filename.length, 26);
    const h = Buffer.alloc(46),
      extra = Buffer.alloc(28);
    h.writeUInt32LE(0x02014b50);
    h.writeUInt16LE(45, 6);
    h.writeUInt16LE(8, 10);
    h.writeUInt32LE(crc, 16);
    h.writeUInt32LE(0xffffffff, 20);
    h.writeUInt32LE(0xffffffff, 24);
    h.writeUInt16LE(filename.length, 28);
    h.writeUInt16LE(extra.length, 30);
    h.writeUInt32LE(0xffffffff, 42);
    extra.writeUInt16LE(1);
    extra.writeUInt16LE(24, 2);
    extra.writeBigUInt64LE(BigInt(data.length), 4);
    extra.writeBigUInt64LE(BigInt(compressed.length), 12);
    extra.writeBigUInt64LE(BigInt(offset), 20);
    locals.push(local, filename, compressed);
    central.push(h, filename, extra);
    offset += local.length + filename.length + compressed.length;
  }
  const directory = Buffer.concat(central),
    end = Buffer.alloc(56),
    locator = Buffer.alloc(20),
    eocd = Buffer.alloc(22);
  end.writeUInt32LE(0x06064b50);
  end.writeBigUInt64LE(44n, 4);
  end.writeBigUInt64LE(BigInt(Object.keys(files).length), 24);
  end.writeBigUInt64LE(BigInt(Object.keys(files).length), 32);
  end.writeBigUInt64LE(BigInt(directory.length), 40);
  end.writeBigUInt64LE(BigInt(offset), 48);
  locator.writeUInt32LE(0x07064b50);
  locator.writeBigUInt64LE(BigInt(offset + directory.length), 8);
  locator.writeUInt32LE(1, 16);
  eocd.writeUInt32LE(0x06054b50);
  eocd.writeUInt16LE(65535, 8);
  eocd.writeUInt16LE(65535, 10);
  eocd.writeUInt32LE(0xffffffff, 12);
  eocd.writeUInt32LE(0xffffffff, 16);
  return Buffer.concat([...locals, directory, end, locator, eocd]);
}
async function put(root: string, name: string, data: Uint8Array | string) {
  const path = join(root, name);
  await mkdir(join(path, ".."), { recursive: true });
  await writeFile(path, data);
  return path;
}
function chunkedFiles() {
  return {
    [`Contents/registered/00000001/${version.name}/00`]: version.data.subarray(0, 3137),
    [`Contents/registered/00000001/${version.name}/01`]: version.data.subarray(3137),
    [`Contents/registered/00000002/${mii.name.replace(".nca", ".cnmt.nca")}`]: mii.data,
  };
}
function encryptedSystem() {
  return nandImage().subarray(0x8000, 0x18000);
}

test("deep registered tree accepts loose NCA and metadata names", async () =>
  workspace(async (root, keys) => {
    for (const file of [version, mii])
      await put(
        root,
        `dump/Contents/registered/00000001/deeper/${file.name.replace(".nca", ".cnmt.nca")}`,
        file.data,
      );
    const fw = await prepareFirmware(join(root, "dump"), keys, join(root, "out"));
    expect(fw.version).toBe("18.1.0");
    expect(fw.files.every((f) => f.name.endsWith(".cnmt.nca"))).toBe(true);
  }));
for (const format of ["folder", "stored ZIP", "deflated ZIP", "ZIP64"] as const)
  test(`chunked NCA directories and mixed metadata in ${format}`, async () =>
    workspace(async (root, keys) => {
      const files = chunkedFiles();
      let source: string;
      if (format === "folder") {
        source = join(root, "dump");
        for (const [name, data] of Object.entries(files)) await put(source, name, data);
      } else {
        source = await put(
          root,
          "dump.zip",
          format === "ZIP64"
            ? zip64(files)
            : zipSync(files, { level: format === "stored ZIP" ? 0 : 6 }),
        );
      }
      const fw = await prepareFirmware(source, keys, join(root, "out"));
      expect(fw.version).toBe("18.1.0");
      expect(fw.files).toHaveLength(2);
      expect(
        (await readFile(fw.files.find((f) => f.name === version.name)!.path)).equals(version.data),
      ).toBe(true);
    }));
for (const bad of ["gap", "unexpected", "corrupt"] as const)
  test(`rejects ${bad} NCA chunks`, async () =>
    workspace(async (root, keys) => {
      const folder = join(root, "dump");
      for (const [name, data] of Object.entries(chunkedFiles())) {
        const changed =
          bad === "gap"
            ? name.replace(/\/01$/, "/02")
            : bad === "unexpected"
              ? name.replace(/\/01$/, "/notes.txt")
              : name;
        await put(
          folder,
          changed,
          bad === "corrupt" && name.endsWith("/01") ? Buffer.alloc(data.length) : data,
        );
      }
      await expect(prepareFirmware(folder, keys, join(root, "out"))).rejects.toThrow(
        "firmwareInvalid",
      );
    }));
for (const selection of ["drive root", "emuMMC", "eMMC", "part", "ZIP64"] as const)
  test(`file-based emuMMC from ${selection} ignores boot partitions`, async () =>
    workspace(async (root, keys) => {
      const nand = nandImage();
      await put(root, "drive/emuMMC/SD00/eMMC/00", nand.subarray(0, 0x9123));
      await put(root, "drive/emuMMC/SD00/eMMC/01", nand.subarray(0x9123));
      await put(root, "drive/emuMMC/SD00/eMMC/boot0", "ignored");
      await put(root, "drive/emuMMC/SD00/eMMC/boot1", "ignored");
      await put(root, "drive/emuMMC/emummc.ini", "[emummc]\npath=emuMMC/SD00\nsector=0\n");
      const source =
        selection === "ZIP64"
          ? await put(
              root,
              "dump.zip",
              zip64({
                "emuMMC/SD00/eMMC/00": nand.subarray(0, 0x9123),
                "emuMMC/SD00/eMMC/01": nand.subarray(0x9123),
                "emuMMC/SD00/eMMC/boot0": Buffer.alloc(10),
              }),
            )
          : join(
              root,
              "drive",
              selection === "drive root"
                ? ""
                : selection === "emuMMC"
                  ? "emuMMC"
                  : `emuMMC/SD00/eMMC${selection === "part" ? "/00" : ""}`,
            );
      expect((await prepareFirmware(source, keys, join(root, "out"))).version).toBe("18.1.0");
    }));
for (const encrypted of [false, true])
  for (const name of ["SYSTEM", "SYSTEM.bin", "system.zip"])
    test(`${encrypted ? "encrypted" : "plain"} standalone ${name}`, async () =>
      workspace(async (root, keys) => {
        const image = encrypted ? encryptedSystem() : fatImage();
        if (!encrypted)
          await writeFile(
            keys,
            `header_key = ${headerKey.toString("hex")}\nkey_area_key_application_00 = ${area.toString("hex")}\n`,
          );
        const source = await put(
          root,
          name,
          name.endsWith(".zip") ? zip64({ "backup/SYSTEM": image }) : image,
        );
        expect((await prepareFirmware(source, keys, join(root, "out"))).version).toBe("18.1.0");
      }));
test("missing emuMMC splits and multiple backups never concatenate unrelated images", async () =>
  workspace(async (root, keys) => {
    const folder = join(root, "dump");
    await put(folder, "emuMMC/SD00/eMMC/00", nandImage());
    await put(folder, "emuMMC/SD00/eMMC/02", "gap");
    await expect(prepareFirmware(folder, keys, join(root, "out"))).rejects.toThrow("nandInvalid");
    await rm(join(folder, "emuMMC/SD00/eMMC/02"));
    await put(folder, "emuMMC/SD01/eMMC/00", nandImage());
    await expect(prepareFirmware(folder, keys, join(root, "out2"))).rejects.toThrow("nandInvalid");
  }));
test("unsupported archives and partition emuMMC explain the next step in both languages", async () =>
  workspace(async (root, keys) => {
    for (const extension of ["7z", "rar"]) {
      const path = await put(root, `firmware.${extension}`, "unsupported");
      const code = extension === "7z" ? "firmware7z" : "firmwareRar";
      await expect(prepareFirmware(path, keys, join(root, extension))).rejects.toMatchObject({
        code,
      });
      expect(strings.en[code]).toContain(`Extract the .${extension} first`);
      expect(strings.pt[code]).toContain(`Extraia o .${extension} primeiro`);
    }
    await put(root, "drive/emuMMC/emummc.ini", "[emummc]\nsector=0x1234\npath=emuMMC/RAW1\n");
    await expect(
      prepareFirmware(join(root, "drive"), keys, join(root, "partition")),
    ).rejects.toMatchObject({ code: "firmwarePartition" });
    for (const language of ["pt", "en"] as const) {
      const report = await checkDrive(join(root, "drive"), language);
      expect(report.items.find((i) => i.id === "firmware")!.fix).toBe(
        strings[language].firmwarePartition,
      );
      expect(strings[language].firmwarePartition).toContain("Hekate");
    }
  }));
for (const form of [
  "chunks",
  "chunks ZIP",
  "NXbox emuMMC",
  "emuMMC",
  "SYSTEM",
  "SYSTEM.bin",
] as const)
  test(`drive detects and fixes ${form} with production verifier, preserving sources`, async () =>
    workspace(async (root, keys) => {
      const drive = join(root, "drive");
      await put(drive, "NXbox/prod.keys", await readFile(keys));
      const originals = new Map<string, Buffer>();
      async function original(name: string, data: Buffer) {
        originals.set(await put(drive, name, data), data);
      }
      if (form === "chunks")
        for (const [name, data] of Object.entries(chunkedFiles()))
          await original(`NXbox/firmware/${name}`, data);
      else if (form === "chunks ZIP") await original("dump.zip", zip64(chunkedFiles()));
      else if (form.includes("emuMMC")) {
        const prefix = form === "NXbox emuMMC" ? "NXbox/" : "";
        const nand = nandImage();
        await original(`${prefix}emuMMC/SD00/eMMC/00`, nand.subarray(0, 0x7123));
        await original(`${prefix}emuMMC/SD00/eMMC/01`, nand.subarray(0x7123));
      } else await original(form, form === "SYSTEM" ? fatImage() : encryptedSystem());
      const before = await checkDrive(drive);
      expect(hasFixableFirmware(before)).toBe(true);
      const after = await fixDrive(drive, before);
      expect(after.items.find((i) => i.id === "firmware")!.status).toBe("ok");
      expect(hasFixableFirmware(after)).toBe(false);
      const files = (await readdir(join(drive, "NXbox/firmware"))).filter((n) =>
        n.endsWith(".nca"),
      );
      expect(files).toHaveLength(2);
      for (const [path, data] of originals) expect((await readFile(path)).equals(data)).toBe(true);
    }));
test("prepare USB and send over Portal both use verified loose NCAs from emuMMC", async () =>
  workspace(async (root, keys) => {
    const source = join(root, "source");
    await put(source, "emuMMC/SD00/eMMC/00", nandImage());
    const game = Buffer.alloc(16);
    game.write("PFS0");
    const gamePath = await put(root, "Game [0100123456789000].nsp", game);
    const inputs = { games: [gamePath], keys, firmware: source };
    const drive = join(root, "drive");
    await mkdir(drive);
    expect((await prepareDrive(drive, inputs)).ready).toBe(true);
    const uploaded: string[] = [];
    await sendToNxbox(
      {
        async packages() {
          return [{ Name: "NXbox", PackageFullName: "NSPX.NXbox_1.0.0.0_x64" }];
        },
        async makeFolder() {},
        async pushFile(_pkg, path, directory, name) {
          if (directory !== "LocalState/firmware") return;
          uploaded.push(name!);
          expect(
            (await readFile(path)).equals(name === version.name ? version.data : mii.data),
          ).toBe(true);
        },
      },
      inputs,
    );
    expect(uploaded.sort()).toEqual([version.name, mii.name].sort());
  }));

test("top-level NCA chunk directories are accepted directly and fixable under firmware/", async () =>
  workspace(async (root, keys) => {
    const drive = join(root, "drive"),
      source = join(drive, "firmware");
    await put(drive, "prod.keys", await readFile(keys));
    for (const file of [version, mii]) {
      await put(source, `${file.name}/00`, file.data.subarray(0, 1000));
      await put(source, `${file.name}/01`, file.data.subarray(1000));
    }
    expect((await prepareFirmware(source, keys, join(root, "out"))).version).toBe("18.1.0");
    const report = await checkDrive(drive);
    expect(hasFixableFirmware(report)).toBe(true);
    expect((await fixDrive(drive, report)).items.find((i) => i.id === "firmware")!.status).toBe(
      "ok",
    );
    expect(
      (await readFile(join(source, `${version.name}/01`))).equals(version.data.subarray(1000)),
    ).toBe(true);
  }));
test("encrypted standalone SYSTEM requires BIS keys and accepts BE sector tweaks", async () =>
  workspace(async (root, keys) => {
    const source = await put(root, "SYSTEM.bin", nandImage(true).subarray(0x8000, 0x18000));
    expect((await prepareFirmware(source, keys, join(root, "out"))).version).toBe("18.1.0");
    await writeFile(keys, `header_key = ${headerKey.toString("hex")}`);
    await expect(prepareFirmware(source, keys, join(root, "missing-bis"))).rejects.toThrow(
      "firmwareKeys",
    );
  }));
test("explicit NAND selection ignores a separate SYSTEM export beside the backup", async () =>
  workspace(async (root, keys) => {
    const source = await put(root, "rawnand.bin", nandImage());
    await put(root, "SYSTEM", fatImage());
    expect((await prepareFirmware(source, keys, join(root, "out"))).version).toBe("18.1.0");
  }));
test("raw-based emuMMC marker gives Hekate guidance without an ini file", async () =>
  workspace(async (root, keys) => {
    const folder = join(root, "emuMMC");
    await put(folder, "RAW1/raw_based", Buffer.alloc(4));
    await expect(prepareFirmware(folder, keys, join(root, "out"))).rejects.toThrow(
      "firmwarePartition",
    );
  }));

test("destination NCA chunk directories are preserved when loose output would collide", async () =>
  workspace(async (root, keys) => {
    const drive = join(root, "drive");
    await put(drive, "prod.keys", await readFile(keys));
    for (const file of [version, mii])
      await put(drive, `NXbox/firmware/${file.name}/00`, file.data);
    const before = await checkDrive(drive);
    expect(hasFixableFirmware(before)).toBe(true);
    const after = await fixDrive(drive, before);
    expect(after.items.find((i) => i.id === "firmware")!.fix).toBe(strings.en.driveConflict);
    for (const file of [version, mii])
      expect(
        (await readFile(join(drive, `NXbox/firmware/${file.name}/00`))).equals(file.data),
      ).toBe(true);
  }));
