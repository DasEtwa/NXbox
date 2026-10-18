// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { expect, test } from "bun:test";
import { mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { zipSync } from "fflate";
import { prepareSaves, prepareMods, prepareTitleKeys } from "../src/optional";
import { renderChecklist } from "../src/checklist";
import { strings } from "../src/i18n";
import { deployGame } from "../src/deploy";
import { DevicePortal } from "../src/portal";
import { inputFiles } from "../src/storage";

const title = "01007EF00011E000";
async function workspace(run: (root: string) => Promise<void>) {
  const root = await mkdtemp(join(tmpdir(), "nxbox-pack-optional-test-"));
  try {
    await run(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}
test("save folder preserves data directories and seeds the exact Eden profile/save contract", async () =>
  workspace(async (root) => {
    const source = join(root, `Backup [${title}]`);
    await mkdir(join(source, "data"), { recursive: true });
    await writeFile(join(source, "data/main"), "synthetic save");
    const result = await prepareSaves(source, title, join(root, "stage"));
    const profile = await readFile(result.files[0]!.path);
    expect(profile.length).toBe(0x650);
    expect(profile.subarray(0x10, 0x20)).toEqual(profile.subarray(0x20, 0x30));
    expect(profile.toString("ascii", 0x38, 0x3e)).toBe("Player");
    const user =
      profile.readBigUInt64LE(0x18).toString(16).padStart(16, "0") +
      profile.readBigUInt64LE(0x10).toString(16).padStart(16, "0");
    expect(result.files[1]!.destination).toBe(
      `LocalState/eden/nand/user/save/0000000000000000/${user.toUpperCase()}/${title}/data/main`,
    );
    const zip = join(root, "Backup.zip");
    await writeFile(zip, zipSync({ "Backup/data/main": Buffer.from("synthetic") }));
    expect(
      (await prepareSaves(zip, title, join(root, "zip-stage"))).files[1]!.destination.endsWith(
        "/data/main",
      ),
    ).toBe(true);
  }));
test("mods accept title layouts, direct romfs/exefs and reject foreign titles or missing layout", async () =>
  workspace(async (root) => {
    const zip = join(root, "mods.zip");
    await writeFile(
      zip,
      zipSync({
        [`load/${title}/Visual/romfs/data/file`]: Buffer.from("mod"),
        [`load/${title}/Visual/exefs/main.ips`]: Buffer.from("patch"),
      }),
    );
    const result = await prepareMods(zip, title, join(root, "stage"));
    expect(result.files.map((f) => f.destination).sort()).toEqual([
      `LocalState/eden/load/${title}/Visual/exefs/main.ips`,
      `LocalState/eden/load/${title}/Visual/romfs/data/file`,
    ]);
    const direct = join(root, "romfs");
    await mkdir(direct);
    await writeFile(join(direct, "file"), "mod");
    expect(
      (await prepareMods(direct, title, join(root, "direct-stage"))).files[0]!.destination,
    ).toBe(`LocalState/eden/load/${title}/Imported/romfs/file`);
    const foreign = join(root, "foreign.zip");
    await writeFile(foreign, zipSync({ "0100AAAA0011E000/Mod/romfs/file": Buffer.from("mod") }));
    await expect(prepareMods(foreign, title, join(root, "foreign-stage"))).rejects.toThrow(
      "optionalMismatch",
    );
    const invalid = join(root, "invalid.zip");
    await writeFile(invalid, zipSync({ file: Buffer.from("mod") }));
    await expect(prepareMods(invalid, title, join(root, "invalid-stage"))).rejects.toThrow(
      "modsInvalid",
    );
  }));
test("unsafe archives, case collisions, symlinks and stored CRC corruption fail", async () =>
  workspace(async (root) => {
    for (const [name, payload] of [
      ["case", { "Mod/romfs/A": Buffer.from("a"), "Mod/romfs/a": Buffer.from("b") }],
      ["unsafe", { "../private": Buffer.from("x") }],
    ] as const) {
      const path = join(root, `${name}.zip`);
      await writeFile(path, zipSync(payload));
      await expect(inputFiles(path, join(root, name))).rejects.toThrow("inputInvalid");
    }
    const data = Buffer.from(zipSync({ file: Buffer.from("synthetic") }, { level: 0 }));
    data[34] = data[34]! ^ 1;
    const corrupt = join(root, "corrupt.zip");
    await writeFile(corrupt, data);
    await expect(inputFiles(corrupt, join(root, "corrupt"))).rejects.toThrow("inputInvalid");
  }));
test("Portuguese and English checklists keep all optional rows and firmware version", () => {
  for (const lang of ["en", "pt"] as const) {
    const text = strings[lang],
      out = renderChecklist(text, { titleId: title, size: 1 }, true, [], {
        firmware: `18.1.0 — 229 ${text.fileCount}`,
        saves: "1",
        mods: "2",
        titleKeys: "title.keys",
      });
    for (const label of [text.itemFirmware, text.itemSaves, text.itemMods, text.itemTitleKeys])
      expect(out).toContain(`[x] ${label}`);
    expect(out).toContain("18.1.0 — 229");
    expect(renderChecklist(text, { titleId: title, size: 1 }, true, [])).toContain(
      `[ ] ${text.itemFirmware} -`,
    );
  }
});
test("optional transfers create every parent first and precede boot markers; failures stop boot", async () =>
  workspace(async (root) => {
    const calls: string[] = [],
      folders = new Set(["LocalState"]);
    let fail = false,
      existing = false;
    class Portal extends DevicePortal {
      override async installAsGames() {
        return false;
      }
      override async uploadRuntime() {}
      override async register() {
        return { Name: "game", PackageFullName: "pkg" };
      }
      override async makeFolder(_pkg: string, parent: string, name: string) {
        expect(folders.has(parent)).toBe(true);
        folders.add(`${parent}/${name}`);
        calls.push(`folder:${parent}/${name}`);
      }
      override async listFiles() {
        return existing ? [{ Name: "profiles.dat" }] : [];
      }
      override async pushFile(_pkg: string, _path: string, directory: string, name = "") {
        expect(folders.has(directory)).toBe(true);
        calls.push(`file:${directory}/${name}`);
        if (fail && directory.endsWith("firmware")) throw new Error("Disk full");
      }
    }
    const portal = new Portal("https://test", { user: "u", password: "p" });
    const transfers = [
      { path: "fw", destination: "LocalState/firmware/fixture.nca" },
      {
        path: "save",
        destination: `LocalState/eden/nand/user/save/0000000000000000/USER/${title}/main`,
      },
      { path: "mod", destination: `LocalState/eden/load/${title}/Mod/romfs/file` },
      { path: "tk", destination: "LocalState/eden/keys/title.keys" },
    ];
    const options = {
      portal,
      titleId: title,
      runtimeDirectory: root,
      runtimeFiles: [],
      content: ["base.nsp"],
      keys: "prod.keys",
      temporary: root,
      transfers,
    };
    await deployGame(options);
    expect(calls.indexOf("folder:LocalState/firmware")).toBeLessThan(
      calls.indexOf("file:LocalState/firmware/fixture.nca"),
    );
    expect(calls.slice(-2)).toEqual(["file:LocalState/", "file:LocalState/"]);
    calls.length = 0;
    fail = true;
    await expect(deployGame(options)).rejects.toThrow("Disk full");
    expect(calls.some((c) => c === "file:LocalState/")).toBe(false);
    calls.length = 0;
    fail = false;
    existing = true;
    await expect(
      deployGame({
        ...options,
        transfers: [
          {
            path: "profile",
            destination:
              "LocalState/eden/nand/system/save/8000000000000010/su/avators/profiles.dat",
          },
        ],
      }),
    ).rejects.toThrow("saveExists");
    expect(calls.some((c) => c.includes("file:") && c.endsWith("profiles.dat"))).toBe(false);
  }));

test("title.keys is optional private content and missing, empty or renamed keys are rejected", async () =>
  workspace(async (root) => {
    const path = join(root, "title.keys");
    await expect(prepareTitleKeys(path)).rejects.toThrow("titleKeysInvalid");
    await writeFile(path, "");
    await expect(prepareTitleKeys(path)).rejects.toThrow("titleKeysInvalid");
    await writeFile(path, "synthetic = 00");
    expect(await prepareTitleKeys(path)).toEqual({
      path,
      destination: "LocalState/eden/keys/title.keys",
    });
    await expect(prepareTitleKeys(join(root, "prod.keys"))).rejects.toThrow("titleKeysInvalid");
  }));
