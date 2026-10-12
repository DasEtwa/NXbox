import { expect, test } from "bun:test";
import { createCipheriv, createHash } from "node:crypto";
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
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
