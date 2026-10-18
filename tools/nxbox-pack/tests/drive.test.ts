import { afterEach, beforeEach, expect, test } from "bun:test";
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { zipSync } from "fflate";
import {
  checkDrive,
  fixDrive,
  hasFixableFirmware,
  prepareDrive,
  sendToNxbox,
  type DriveInputs,
  type DriveServices,
  type NxboxPortal,
} from "../src/drive";
import { inputFiles } from "../src/storage";
import { PackError } from "../src/i18n";
let root: string, source: string;
const keyText = `header_key = ${"12".repeat(32)}\n`;
const firmwareName = `${"a".repeat(32)}.nca`;
const deps: DriveServices = {
  async verify(files) {
    if (!files.length) throw new PackError("firmwareIncomplete");
    return { files, version: "20.0.0" };
  },
  async prepare(path, _keys, temporary) {
    return { files: await inputFiles(path, temporary), version: "20.0.0" };
  },
};
function pfs() {
  const b = Buffer.alloc(48);
  b.write("PFS0");
  b.writeUInt32LE(1, 4);
  b.writeUInt32LE(4, 8);
  b.writeBigUInt64LE(4n, 24);
  b.write("a\0", 40);
  return b;
}
async function put(base: string, name: string, data: string | Uint8Array = "data") {
  const path = join(base, name);
  await mkdir(join(path, ".."), { recursive: true });
  await writeFile(path, data);
  return path;
}
async function correct() {
  await put(root, "prod.keys", keyText);
  await put(root, `NXbox/firmware/${firmwareName}`);
  await put(root, "NXbox/games/Game [0100123456789000].nsp", pfs());
}
const item = (report: Awaited<ReturnType<typeof checkDrive>>, id: string) =>
  report.items.find((i) => i.id === id)!;
async function inputs(): Promise<DriveInputs> {
  return {
    keys: await put(source, "prod.keys", keyText),
    titleKeys: await put(source, "title.keys", `${"a".repeat(32)} = ${"b".repeat(32)}`),
    firmware: join(source, "firmware"),
    games: [await put(source, "Game.NSP", pfs())],
  };
}
beforeEach(async () => {
  root = await mkdtemp(join(tmpdir(), "nxhelper-drive-test-"));
  source = await mkdtemp(join(tmpdir(), "nxhelper-input-test-"));
  await put(source, `firmware/${firmwareName}`);
});
afterEach(async () => {
  await rm(root, { recursive: true, force: true });
  await rm(source, { recursive: true, force: true });
});
test("keys use NXbox, then switch, then root; wrong location fails with a move fix", async () => {
  await correct();
  expect(item(await checkDrive(root, "en", deps), "prodKeys").detail).toContain("prod.keys");
  await put(root, "switch/prod.keys", keyText);
  expect(item(await checkDrive(root, "en", deps), "prodKeys").detail).toContain("switch");
  await put(root, "NXbox/prod.keys", keyText);
  expect(item(await checkDrive(root, "en", deps), "prodKeys").detail).toContain("NXbox");
  await rm(join(root, "NXbox/prod.keys"));
  await rm(join(root, "prod.keys"));
  await rm(join(root, "switch"), { recursive: true });
  await put(root, "keys/prod.keys", keyText);
  const wrong = item(await checkDrive(root, "en", deps), "prodKeys");
  expect(wrong.status).toBe("fail");
  expect(wrong.detail).toContain("keys");
  expect(wrong.fix).toContain("drive root");
});
test("missing optional title keys warns, valid layout is ready", async () => {
  await correct();
  const report = await checkDrive(root, "en", deps);
  expect(item(report, "titleKeys").status).toBe("warn");
  expect(report.ready).toBe(true);
  expect(item(report, "firmware").detail).toBe("Version 20.0.0, 1 files");
});
test("firmware zip is fixable and fixing preserves original and writes loose NCAs", async () => {
  await correct();
  await rm(join(root, "NXbox/firmware"), { recursive: true });
  const archive = zipSync({ [`dump/nested/${firmwareName}`]: new Uint8Array([1, 2, 3]) });
  await put(root, "firmware.zip", archive);
  const report = await checkDrive(root, "en", deps);
  expect(hasFixableFirmware(report)).toBe(true);
  expect(report.ready).toBe(false);
  expect((await fixDrive(root, report, "en", deps)).ready).toBe(true);
  expect(await readFile(join(root, `NXbox/firmware/${firmwareName}`))).toEqual(
    Buffer.from([1, 2, 3]),
  );
  expect(await readFile(join(root, "firmware.zip"))).toEqual(Buffer.from(archive));
});
test("deep firmware can be flattened without removing originals", async () => {
  await correct();
  await rm(join(root, "NXbox/firmware"), { recursive: true });
  await put(root, `firmware/dump/deeper/${firmwareName}`);
  const report = await checkDrive(root, "en", deps);
  expect(hasFixableFirmware(report)).toBe(true);
  expect((await fixDrive(root, report, "en", deps)).ready).toBe(true);
});
test("truncated PFS0 fails, uppercase warns, root game warns about layout", async () => {
  await correct();
  await put(root, "games/Broken.nsp", pfs().subarray(0, 47));
  await put(root, "games/Upper.NSP", pfs());
  await put(root, "Root.xci");
  const report = await checkDrive(root, "pt", deps);
  expect(item(report, "gameIncomplete").status).toBe("fail");
  expect(item(report, "gameExtension").status).toBe("warn");
  expect(item(report, "layout").fix).toContain("NXbox/games");
  expect(report.ready).toBe(false);
});
test("production verifier rejects fake firmware and invalid header keys", async () => {
  await correct();
  expect(item(await checkDrive(root), "firmware").status).toBe("fail");
  await put(root, "prod.keys", "header_key = aa");
  expect(item(await checkDrive(root), "prodKeys").status).toBe("fail");
});
test("firmware errors map to actionable fixes", async () => {
  await correct();
  for (const code of ["firmwareKeys", "firmwareInvalid", "firmwareIncomplete"] as const) {
    const report = await checkDrive(root, "en", {
      ...deps,
      verify: async () => {
        throw new PackError(code);
      },
    });
    expect(item(report, "firmware").fix).toContain(
      code === "firmwareKeys" ? "updated Switch" : "whole firmware",
    );
  }
});
test("prepareDrive creates exact layout, lowercase extension and ready report", async () => {
  const progress: string[] = [];
  const report = await prepareDrive(root, await inputs(), (f) => progress.push(f), deps);
  expect(report.ready).toBe(true);
  // Everything lives in NXbox/, so a drive shared with other consoles stays tidy.
  expect(await readdir(root)).toEqual(["NXbox"]);
  expect((await readdir(join(root, "NXbox"))).sort()).toEqual([
    "firmware",
    "games",
    "prod.keys",
    "title.keys",
  ]);
  expect(await readdir(join(root, "NXbox/games"))).toEqual(["Game.nsp"]);
  expect(await readdir(join(root, "NXbox/firmware"))).toEqual([firmwareName]);
  expect(progress).toContain("NXbox/games/Game.nsp");
});
test("xcz refused before copying and collisions preflight all files", async () => {
  const input = await inputs();
  input.games.push(await put(source, "Other.xcz"));
  await expect(prepareDrive(root, input, undefined, deps)).rejects.toMatchObject({
    code: "driveConvert",
  });
  expect(await readdir(root)).toEqual([]);
  input.games.pop();
  await put(root, "NXbox/games/Game.nsp", "different");
  await expect(prepareDrive(root, input, undefined, deps)).rejects.toMatchObject({
    code: "driveConflict",
  });
  expect(await readdir(root)).toEqual(["NXbox"]);
});
test("same size destination is skipped and preserved", async () => {
  const input = await inputs();
  await prepareDrive(root, input, undefined, deps);
  await expect(prepareDrive(root, input, undefined, deps)).resolves.toMatchObject({ ready: true });
});
test("portal chooses newest library package, uploads games, keys, firmware in order", async () => {
  const pushed: string[] = [],
    selected: string[] = [];
  const portal: NxboxPortal = {
    async packages() {
      return [
        "NSPX.NXbox_2.0.0.0_x64",
        "NSPX.NXbox_10.0.0.0_x64",
        "NSPX.NXboxGame_99.0.0.0_x64",
      ].map((PackageFullName) => ({ Name: "NXbox", PackageFullName }));
    },
    async makeFolder() {},
    async pushFile(pkg, _path, directory, name) {
      selected.push(pkg);
      pushed.push(`${directory}/${name}`);
    },
  };
  const result = await sendToNxbox(portal, await inputs(), undefined, deps);
  expect(new Set(selected)).toEqual(new Set(["NSPX.NXbox_10.0.0.0_x64"]));
  expect(pushed).toEqual([
    "LocalState/games/Game.nsp",
    "LocalState/eden/keys/prod.keys",
    "LocalState/eden/keys/title.keys",
    `LocalState/firmware/${firmwareName}`,
  ]);
  expect(result.warnings[0]).toContain("SSD");
  portal.packages = async () => [];
  await expect(sendToNxbox(portal, await inputs(), undefined, deps)).rejects.toMatchObject({
    code: "nxboxMissing",
  });
  portal.packages = async () => [{ Name: "NXbox", PackageFullName: "NSPX.NXbox_1.0.0.0_x64" }];
  portal.freeSpace = async () => 0;
  await expect(sendToNxbox(portal, await inputs(), undefined, deps)).rejects.toMatchObject({
    code: "driveSpace",
  });
});

test("NAND splits are offered for repair; failed firmware repair returns an actionable report", async () => {
  await correct();
  await rm(join(root, "NXbox/firmware"), { recursive: true });
  await put(root, "rawnand.bin.00");
  const report = await checkDrive(root, "en", deps);
  expect(hasFixableFirmware(report)).toBe(true);
  const failure = await fixDrive(root, report, "en", {
    ...deps,
    prepare: async () => {
      throw new PackError("firmwareKeys");
    },
  });
  expect(failure.ready).toBe(false);
  expect(item(failure, "firmware").fix).toContain("updated Switch");
  expect(await readFile(join(root, "rawnand.bin.00"), "utf8")).toBe("data");
});
test("keys go into NXbox/ ahead of stale switch keys; a destination directory fails first", async () => {
  const input = await inputs();
  await put(root, "switch/prod.keys", `header_key = ${"ab".repeat(32)}`);
  const report = await prepareDrive(root, input, undefined, deps);
  expect(item(report, "prodKeys").detail).toContain("NXbox");
  expect(await readFile(join(root, "NXbox/prod.keys"), "utf8")).toBe(keyText);
  await rm(join(root, "switch"), { recursive: true });
  await rm(join(root, "NXbox"), { recursive: true });
  await mkdir(join(root, "NXbox/games/Game.nsp"), { recursive: true });
  await expect(prepareDrive(root, input, undefined, deps)).rejects.toMatchObject({
    code: "driveConflict",
  });
  expect(await readdir(root)).toEqual(["NXbox"]);
});

test("one problem item per game combines fixes and keeps failure severity", async () => {
  await correct();
  await put(root, "games/Partial.NSP", pfs().subarray(0, 47));
  const report = await checkDrive(root, "en", deps);
  const matches = report.items.filter((i) => i.title === "games/Partial.NSP");
  expect(matches).toHaveLength(1);
  expect(matches[0]!.status).toBe("fail");
  expect(matches[0]!.fix).toContain("lowercase");
  expect(matches[0]!.fix).toContain("Incomplete game");
  expect(matches[0]!.fix).toContain("title ID");
});
