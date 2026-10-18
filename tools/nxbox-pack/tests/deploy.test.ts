// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { expect, test } from "bun:test";
import { zipSync } from "fflate";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { stageRuntime } from "../src/runtime";
import { DevicePortal } from "../src/portal";
import { deployGame } from "../src/deploy";

function runtimeFixture(): Record<string, Uint8Array> {
  const executable = Buffer.alloc(128);
  executable.write("MZ");
  executable.writeUInt32LE(64, 0x3c);
  executable.set([80, 69, 0, 0, 100, 134], 64);
  return Object.fromEntries(
    [
      "nxbox.exe",
      "opengl32.dll",
      "libgallium_wgl.dll",
      "libglapi.dll",
      "dxil.dll",
      "game.nro",
      "Assets/StoreLogo.png",
      "Assets/Square44x44Logo.png",
      "Assets/Square150x150Logo.png",
      "Notices/Eden-LICENSE.txt",
      "Notices/Mesa-LICENSE.rst",
      "Notices/DXC-LICENSE-MS.txt",
      "Notices/DXC-LICENSE-LLVM.txt",
    ].map((name) => [name, name === "nxbox.exe" ? executable : Buffer.from("fixture")]),
  );
}
test("staging through registration and verified content uploads completes through the HTTP transport", async () => {
  const titleId = "01007EF00011E000";
  const identity = `NSPX.NXbox.${titleId}`;
  const installed = { Name: "Game", PackageFullName: `${identity}_0.2.0.0_x64__xb0tjvbn8pcrt` };
  const uploads = new Map<string, Uint8Array>();
  const runtime = new Map<string, Uint8Array>();
  const uploadOrder: string[] = [];
  let registered = false;
  // Exercise the same streaming HTTP requests without requiring a listening socket.
  const transport = (async (input, init) => {
    const request = new Request(String(input), init);
    const url = new URL(request.url);
    expect(request.headers.get("Authorization")).toBe(
      `Basic ${Buffer.from("user:password").toString("base64")}`,
    );
    if (request.method === "POST") expect(request.headers.get("X-CSRF-Token")).toBe("token");
    if (url.pathname.endsWith("/machinename"))
      return Response.json(
        { ComputerName: "Xbox" },
        { headers: { "Set-Cookie": "CSRF-Token=token" } },
      );
    if (
      url.pathname.endsWith("/upload") ||
      (url.pathname.endsWith("/file") && request.method === "POST")
    ) {
      const form = await request.formData();
      const file = form.get("file") as File;
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (url.pathname.endsWith("/upload")) {
        const folder = Buffer.from(url.searchParams.get("destinationFolder")!, "base64").toString();
        // Like the console: each upload replaces the files already in that folder.
        for (const key of [...runtime.keys()])
          if (key.slice(0, key.lastIndexOf("\\")) === folder) runtime.delete(key);
        for (const part of form.getAll("file") as File[])
          runtime.set(`${folder}\\${part.name}`, new Uint8Array(await part.arrayBuffer()));
      } else {
        expect(url.searchParams.get("packagefullname")).toBe(installed.PackageFullName);
        const path = `${url.searchParams.get("path")!.replaceAll("\\", "/")}/${file.name}`;
        uploads.set(path, bytes);
        uploadOrder.push(path);
      }
      return new Response("");
    }
    if (url.pathname.endsWith("/register")) {
      registered = true;
      return new Response("");
    }
    if (url.pathname.endsWith("/state")) return Response.json({ Code: 0 });
    if (url.pathname.endsWith("/packages"))
      return Response.json({ InstalledPackages: registered ? [installed] : [] });
    if (url.pathname.endsWith("/folder")) return new Response("");
    if (url.pathname.endsWith("/files")) {
      const prefix = url.searchParams.get("path")!.replaceAll("\\", "/") + "/";
      return Response.json({
        Items: [...uploads]
          .filter(([path]) => path.startsWith(prefix))
          .map(([path, data]) => ({ Name: path.slice(prefix.length), FileSize: data.length })),
      });
    }
    return new Response("Unexpected endpoint", { status: 404 });
  }) as typeof fetch;
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-deploy-test-"));
  try {
    const runtimeDirectory = join(temporary, "runtime");
    const runtimeFiles = await stageRuntime(
      zipSync(runtimeFixture()),
      runtimeDirectory,
      titleId,
      "Fixture Game",
    );
    const keys = join(temporary, "prod.keys");
    await writeFile(keys, "synthetic key fixture");
    const base = join(temporary, "base.nsp");
    await writeFile(base, Buffer.alloc(2 * 1024 * 1024, 7));
    const update = join(temporary, "update.nsp");
    await writeFile(update, "synthetic update");
    const portal = new DevicePortal(
      "https://xbox.test:11443",
      {
        user: "user",
        password: "password",
      },
      false,
      transport,
    );
    expect(
      await deployGame({
        portal,
        titleId,
        runtimeDirectory,
        runtimeFiles,
        content: [base, update],
        keys,
        temporary,
      }),
    ).toBe(identity);
    expect(new TextDecoder().decode(uploads.get("/LocalState/game.txt"))).toBe("games\\base.nsp\n");
    expect(new TextDecoder().decode(uploads.get("/LocalState/skip_library.txt"))).toBe("1\n");
    expect(uploads.get("/LocalState/games/base.nsp")?.length).toBe(2 * 1024 * 1024);
    expect(uploads.has("/LocalState/games/update.nsp")).toBe(true);
    expect(uploads.has("/LocalState/eden/keys/prod.keys")).toBe(true);
    expect(uploadOrder.slice(-2)).toEqual(["/LocalState/game.txt", "/LocalState/skip_library.txt"]);
    const names = [...runtime.keys()];
    // Every staged runtime file must survive on the console, not just the last per folder.
    expect(
      names.map((name) => name.slice(name.indexOf("\\") + 1).replaceAll("\\", "/")).sort(),
    ).toEqual([...runtimeFiles].sort());
    expect(names.some((name) => name.endsWith("prod.keys") || name.endsWith(".nsp"))).toBe(false);
    const manifest = [...runtime].find(([name]) => name.endsWith("AppxManifest.xml"))![1];
    expect(new TextDecoder().decode(manifest)).toContain(identity);
    expect(new TextDecoder().decode(manifest)).not.toContain("windows.protocol");
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("a failed game upload never enables direct boot", async () => {
  const pushed: string[] = [];
  class FailingPortal extends DevicePortal {
    override async installAsGames() {
      return false;
    }
    override async uploadRuntime(): Promise<void> {}
    override async register() {
      return { Name: "game", PackageFullName: "game_full_name" };
    }
    override async makeFolder(): Promise<void> {}
    override async pushFile(_package: string, path: string): Promise<void> {
      pushed.push(path);
      if (path.endsWith(".nsp")) throw new Error("Disk full");
    }
  }
  const portal = new FailingPortal("https://xbox.test:11443", { user: "u", password: "p" });
  await expect(
    deployGame({
      portal,
      titleId: "01007EF00011E000",
      runtimeDirectory: "runtime",
      runtimeFiles: [],
      content: ["base.nsp"],
      keys: "prod.keys",
      temporary: "temporary",
    }),
  ).rejects.toThrow("Disk full");
  expect(pushed).toContain("prod.keys");
  expect(
    pushed.some((path) => path.endsWith("game.txt") || path.endsWith("skip_library.txt")),
  ).toBe(false);
});
