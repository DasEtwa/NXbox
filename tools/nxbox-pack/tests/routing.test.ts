import { expect, test } from "bun:test";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { copyGames } from "../src/api";
import { deployGame } from "../src/deploy";
import { prepareDrive, sendToNxbox } from "../src/drive";
import { destinationProgress, gameDestination, titleFolder } from "../src/files";
import { DevicePortal } from "../src/portal";

const cases = [
  ["Game [0100123456780000].nsp", "Base"],
  ["Patch [0100123456780800].nsp", "Updates"],
  ["Extra [0100123456781001].nsp", "DLC"],
  ["Mystery.nsp", ""],
] as const;
for (const [name, folder] of cases) {
  test(`routes ${folder || "unknown"} through USB, existing app, deployment and local export`, async () => {
    const root = await mkdtemp(join(tmpdir(), "nxbox-routing-"));
    try {
      const source = join(root, name),
        keys = join(root, "prod.keys");
      const bytes = Buffer.alloc(16);
      bytes.write("PFS0");
      await writeFile(source, bytes);
      await writeFile(keys, `header_key = ${"12".repeat(32)}\n`);
      const destination = [folder, name].filter(Boolean).join("/");
      expect(await gameDestination(source, keys)).toBe(destination);
      const progress: string[] = [];
      const onProgress = (file: string) => {
        progress.push(file);
      };
      const out = join(root, "usb");
      await mkdir(out);
      const report = await prepareDrive(out, { games: [source], keys }, onProgress);
      expect(report.items.find((i) => i.id === "games")?.status).toBe("ok");
      expect(await Bun.file(join(out, "NXbox/games", destination)).bytes()).toEqual(bytes);
      const pushed: string[] = [],
        made: string[] = [];
      class Portal extends DevicePortal {
        override async installAsGames() {
          return false;
        }
        override async uploadRuntime() {}
        override async register() {
          return { Name: "NXbox", PackageFullName: "NSPX.NXbox_1" };
        }
        override async packages() {
          return [{ Name: "NXbox", PackageFullName: "NSPX.NXbox_1" }];
        }
        override async freeSpace() {
          return 1e12;
        }
        override async makeFolder(_pkg: string, parent: string, child: string) {
          made.push(`${parent}/${child}`);
        }
        override async pushFile(
          _pkg: string,
          path: string,
          directory: string,
          remoteName = "",
          progress?: (sent: number, total: number) => void,
        ) {
          pushed.push(`${directory}/${remoteName}`);
          progress?.(Bun.file(path).size, Bun.file(path).size);
        }
      }
      const portal = new Portal("https://xbox.test", { user: "", password: "" });
      await sendToNxbox(portal, { games: [source], keys }, onProgress);
      expect(pushed).toContain(`LocalState/games/${destination}`);
      if (folder) expect(made).toContain(`LocalState/games/${folder}`);
      pushed.length = 0;
      await deployGame({
        portal,
        content: [source],
        keys,
        temporary: root,
        titleId: "0100123456780000",
        runtimeDirectory: root,
        runtimeFiles: [],
        onProgress,
      });
      expect(pushed).toContain(`LocalState/games/${destination}`);
      expect(await Bun.file(join(root, "game.txt")).text()).toBe(
        `games\\${destination.replaceAll("/", "\\")}\n`,
      );
      const local = join(root, "local");
      await copyGames([source], local, keys, onProgress);
      expect(await Bun.file(join(local, "games", destination)).bytes()).toEqual(bytes);
      expect(
        progress.some((p) => p.includes(name) && p.includes(folder ? `games/${folder}` : "games")),
      ).toBe(true);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
}

test("destination progress preserves each target folder", () => {
  for (const folder of ["Base", "Updates", "DLC"]) {
    expect(destinationProgress(`games/${folder}/Game.nsp`)).toBe(`Game.nsp → games/${folder}`);
  }
});

test("title classification includes the DLC offset bit", () => {
  for (const [suffix, folder] of [
    ["0000", "Base"],
    ["0800", "Updates"],
    ["1001", "DLC"],
    ["1800", "DLC"],
    ["1fff", "DLC"],
    ["0001", ""],
    ["1000", ""],
  ] as const) {
    expect(titleFolder(`010012345678${suffix}`)).toBe(folder);
  }
});

test("destinations lowercase only the extension", async () => {
  expect(await gameDestination("Game [0100123456780000].NSP")).toBe(
    "Base/Game [0100123456780000].nsp",
  );
});

test("direct boot deployment and local export reject unsafe destinations", async () => {
  class Portal extends DevicePortal {
    override async installAsGames() {
      return false;
    }
    override async uploadRuntime() {}
    override async register() {
      return { Name: "NXbox", PackageFullName: "NSPX.NXbox_1" };
    }
    override async makeFolder() {}
    override async pushFile(_pkg: string, _path: string, directory: string) {
      expect(directory).not.toStartWith("LocalState/games");
    }
  }
  const portal = new Portal("https://xbox.test", { user: "", password: "" });
  for (const name of ["CON.nsp", "Bad:Name.nsp", "Bad?Name.nsp"]) {
    await expect(
      deployGame({
        portal,
        content: [name],
        keys: "",
        temporary: "",
        titleId: "0100123456780000",
        runtimeDirectory: "",
        runtimeFiles: [],
      }),
    ).rejects.toMatchObject({ code: "inputInvalid" });
    await expect(copyGames([name], "", "")).rejects.toMatchObject({ code: "inputInvalid" });
  }
});
