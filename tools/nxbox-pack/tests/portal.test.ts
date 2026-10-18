// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { expect, test } from "bun:test";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { DevicePortal, PART_SIZE, portalUrl } from "../src/portal";

test("portal URL validation keeps credentials out of URLs", () => {
  expect(portalUrl("192.168.1.5")).toBe("https://192.168.1.5:11443");
  expect(portalUrl("xbox.local:11443")).toBe("https://xbox.local:11443");
  for (const url of [
    "http://xbox.local",
    "https://user:pass@xbox.local",
    "https://xbox.local/evil",
  ])
    expect(() => portalUrl(url)).toThrow();
});
test("Xbox loose deployment and multipart LocalState uploads use the portal contract", async () => {
  const calls: { url: URL; method: string; bytes: number; body: string }[] = [];
  let registered = false;
  const transport = (async (input: string | URL | Request, init?: RequestInit) => {
    const url = new URL(String(input));
    const headers = new Headers(init?.headers);
    expect(headers.get("Authorization")).toBe(
      `Basic ${Buffer.from("user:password").toString("base64")}`,
    );
    if (init?.method === "POST") expect(headers.get("X-CSRF-Token")).toBe("token");
    const raw = init?.body
      ? Buffer.from(await new Response(init.body).arrayBuffer())
      : Buffer.alloc(0);
    const bytes = raw.byteLength;
    calls.push({ url, method: init?.method ?? "GET", bytes, body: raw.toString("latin1") });
    if (url.pathname.endsWith("machinename"))
      return Response.json(
        { ComputerName: "Xbox" },
        { headers: { "Set-Cookie": "CSRF-Token=token; Path=/" } },
      );
    if (url.pathname.endsWith("/state")) return Response.json({ Code: 0 });
    if (url.pathname.endsWith("/register")) registered = true;
    if (url.pathname.endsWith("/packages"))
      return Response.json({
        // The previous version stays listed; Name is the display name, not the identity.
        InstalledPackages: [
          {
            Name: "Game",
            PackageFullName: "NSPX.NXbox.01007EF00011E000_0.1.0.0_x64__xb0tjvbn8pcrt",
          },
          ...(registered
            ? [
                {
                  Name: "Game",
                  PackageFullName: "NSPX.NXbox.01007EF00011E000_0.2.0.0_x64__xb0tjvbn8pcrt",
                },
              ]
            : []),
        ],
      });
    if (url.pathname.endsWith("/files"))
      return Response.json({ Items: [{ Name: "base.nsp", FileSize: 4 * 1024 * 1024 }] });
    if (init?.body) expect(Number(headers.get("Content-Length"))).toBe(bytes);
    return new Response("");
  }) as unknown as typeof fetch;
  const portal = new DevicePortal(
    "https://xbox.test:11443",
    { user: "user", password: "password" },
    true,
    transport,
  );
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-portal-test-"));
  try {
    const path = join(temporary, "local.nsp");
    await writeFile(path, Buffer.alloc(4 * 1024 * 1024, 1));
    const runtime = [
      "nxbox.exe",
      "opengl32.dll",
      "Assets/logo.png",
      "Assets/wide.png",
      "Assets/avatars/fox-pilot.png",
    ];
    await mkdir(join(temporary, "Assets/avatars"), { recursive: true });
    for (const file of runtime) await writeFile(join(temporary, file), file);
    await portal.uploadRuntime("game-folder", temporary, runtime);
    const installed = await portal.register("game-folder", "NSPX.NXbox.01007EF00011E000", 5000);
    expect(installed.PackageFullName).toBe(
      "NSPX.NXbox.01007EF00011E000_0.2.0.0_x64__xb0tjvbn8pcrt",
    );
    await portal.makeFolder(installed.PackageFullName, "LocalState", "games");
    let sent = 0;
    await portal.pushFile(installed.PackageFullName, path, "LocalState/games", "base.nsp", (n) => {
      sent = n;
    });
    expect(sent).toBe(4 * 1024 * 1024);
    // The console keeps only the last upload per folder: one request per folder, root first.
    const uploads = calls.filter((c) => c.url.pathname.endsWith("/upload"));
    expect(
      uploads.map((c) =>
        Buffer.from(c.url.searchParams.get("destinationFolder")!, "base64").toString(),
      ),
    ).toEqual(["game-folder", "game-folder\\Assets", "game-folder\\Assets\\avatars"]);
    expect(uploads[0]!.body).toContain('filename="nxbox.exe"');
    expect(uploads[0]!.body).toContain('filename="opengl32.dll"');
    expect(uploads[1]!.body).toContain('filename="logo.png"');
    expect(uploads[1]!.body).toContain('filename="wide.png"');
    expect(uploads[2]!.body).toContain('filename="fox-pilot.png"');
    const parts = await new Response(uploads[1]!.body, {
      headers: {
        "Content-Type": `multipart/form-data; boundary=${uploads[1]!.body.slice(2, uploads[1]!.body.indexOf("\r\n"))}`,
      },
    }).formData();
    expect(parts.getAll("file").length).toBe(2);
    const registration = calls.find((c) => c.url.pathname.endsWith("/register"))!;
    expect(Buffer.from(registration.url.searchParams.get("folder")!, "base64").toString()).toBe(
      "game-folder",
    );
    const file = calls.find((c) => c.url.pathname.endsWith("/file"))!;
    expect(file.url.searchParams.get("path")).toBe("\\LocalState\\games");
    expect(file.url.searchParams.get("packagefullname")).toBe(
      "NSPX.NXbox.01007EF00011E000_0.2.0.0_x64__xb0tjvbn8pcrt",
    );
    expect(file.url.searchParams.get("extract")).toBe("false");
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
test("authentication redirects and HTTP failures do not reach registration", async () => {
  for (const status of [302, 401, 403, 500]) {
    const portal = new DevicePortal(
      "https://xbox.test:11443",
      { user: "u", password: "p" },
      true,
      (async () => new Response("private details", { status })) as unknown as typeof fetch,
    );
    await expect(portal.packages()).rejects.toThrow("portal");
  }
});
test("remote upload size mismatch is an error", async () => {
  const portal = new DevicePortal(
    "https://xbox.test:11443",
    { user: "u", password: "p" },
    true,
    (async () =>
      Response.json(
        { Items: [] },
        { headers: { "Set-Cookie": "CSRF-Token=token" } },
      )) as unknown as typeof fetch,
  );
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-size-test-"));
  try {
    const path = join(temporary, "base.nsp");
    await writeFile(path, "dump");
    await expect(portal.pushFile("pkg", path, "LocalState/games")).rejects.toThrow("portal");
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("a 204 deployment state is pending even if an older package exists", async () => {
  let states = 0;
  const portal = new DevicePortal(
    "https://xbox.test:11443",
    { user: "u", password: "p" },
    true,
    (async (input: string | URL | Request) => {
      const path = new URL(String(input)).pathname;
      if (path.endsWith("/state")) {
        states++;
        return states === 1
          ? new Response(null, { status: 204 })
          : Response.json({ Code: 0, Success: true });
      }
      if (path.endsWith("/packages")) {
        return Response.json({
          InstalledPackages:
            states >= 2 ? [{ Name: "Game", PackageFullName: "game_1.0_x64__x" }] : [],
        });
      }
      return new Response("", { headers: { "Set-Cookie": "CSRF-Token=token" } });
    }) as unknown as typeof fetch,
  );
  expect((await portal.register("folder", "game", 5000)).PackageFullName).toBe("game_1.0_x64__x");
});
test("registration HRESULT failures are not reported as success", async () => {
  const portal = new DevicePortal(
    "https://xbox.test:11443",
    { user: "u", password: "p" },
    true,
    (async (input: string | URL | Request) => {
      if (String(input).includes("/state"))
        return Response.json({ Code: -2147009280, Success: false });
      if (String(input).includes("/packages")) return Response.json({ InstalledPackages: [] });
      return new Response("", { headers: { "Set-Cookie": "CSRF-Token=token" } });
    }) as unknown as typeof fetch,
  );
  await expect(portal.register("folder", "game", 5000)).rejects.toThrow("deploy");
});

test("portal without authentication sends no Authorization header", async () => {
  const { DevicePortal } = await import("../src/portal");
  let seen: string | null = "unset";
  const portal = new DevicePortal("https://xbox:11443", { user: "", password: "" }, true, (async (
    _url: string,
    init: RequestInit,
  ) => {
    seen = new Headers(init.headers).get("Authorization");
    return new Response("[]", { status: 200 });
  }) as unknown as typeof fetch);
  await portal.packages().catch(() => {});
  expect(seen).toBeNull();
});
test("installAsGames turns on the Game content type only when it is off", async () => {
  const puts: { path: string; body: string }[] = [];
  let value = "false";
  const transport = (async (input: string | URL | Request, init?: RequestInit) => {
    const url = new URL(String(input));
    if (url.pathname.endsWith("machinename"))
      return Response.json({}, { headers: { "Set-Cookie": "CSRF-Token=token; Path=/" } });
    if (init?.method === "PUT") {
      puts.push({ path: url.pathname, body: String(init.body) });
      value = "true";
      return new Response("");
    }
    return Response.json({
      Settings: [{ Name: "DefaultUWPContentTypeToGame", Value: value, Type: "Text" }],
    });
  }) as typeof fetch;
  const portal = new DevicePortal(
    "https://xbox.test:11443",
    { user: "user", password: "password" },
    true,
    transport,
  );
  expect(await portal.installAsGames()).toBe(true);
  expect(puts).toEqual([
    { path: "/ext/settings/DefaultUWPContentTypeToGame", body: '{"Value":"true"}' },
  ]);
  expect(await portal.installAsGames()).toBe(false);
  expect(puts.length).toBe(1);
});

/** A console that stores each uploaded file by name and lists them back, like the file API. */
function fakeConsole(shortName?: string) {
  const files = new Map<string, Buffer>();
  const order: string[] = [];
  const transport = (async (input: string | URL | Request, init?: RequestInit) => {
    const url = new URL(String(input));
    if (url.pathname.endsWith("machinename"))
      return Response.json({}, { headers: { "Set-Cookie": "CSRF-Token=token; Path=/" } });
    if (init?.method === "POST") {
      const raw = Buffer.from(await new Response(init.body).arrayBuffer());
      expect(Number(new Headers(init.headers).get("Content-Length"))).toBe(raw.length);
      const text = raw.toString("latin1");
      const boundary = text.slice(2, text.indexOf("\r\n"));
      const name = /filename="([^"]+)"/.exec(text)![1]!;
      files.set(
        name,
        raw.subarray(text.indexOf("\r\n\r\n") + 4, raw.length - `\r\n--${boundary}--\r\n`.length),
      );
      order.push(name);
      return new Response("");
    }
    return Response.json({
      Items: [...files].map(([Name, data]) => ({
        Name,
        FileSize: Name === shortName ? data.length - 1 : data.length,
      })),
    });
  }) as unknown as typeof fetch;
  return {
    files,
    order,
    portal: new DevicePortal(
      "https://xbox.test:11443",
      { user: "u", password: "p" },
      true,
      transport,
    ),
  };
}

test("a file above the part size goes up as numbered parts, then the manifest", async () => {
  expect(PART_SIZE).toBe(1073741824);
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-split-test-"));
  try {
    const path = join(temporary, "big.nsp");
    const data = Buffer.from(Array.from({ length: 10 }, (_, i) => i + 1));
    await writeFile(path, data);
    const { files, order, portal } = fakeConsole();
    const progress: [number, number][] = [];
    await portal.pushFile(
      "pkg",
      path,
      "LocalState/games",
      "base.nsp",
      (sent, total) => {
        progress.push([sent, total]);
      },
      4,
    );
    expect(order).toEqual([
      "base.nsp.nxpart000",
      "base.nsp.nxpart001",
      "base.nsp.nxpart002",
      "base.nsp.nxparts",
    ]);
    expect([...files.get("base.nsp.nxpart000")!]).toEqual([1, 2, 3, 4]);
    expect([...files.get("base.nsp.nxpart001")!]).toEqual([5, 6, 7, 8]);
    expect([...files.get("base.nsp.nxpart002")!]).toEqual([9, 10]);
    expect(files.get("base.nsp.nxparts")!.toString()).toBe("parts=3\nsize=10\n");
    expect(files.has("base.nsp")).toBe(false);
    // Progress covers the whole file and never restarts per part.
    expect(progress.every(([, total]) => total === 10)).toBe(true);
    expect(progress.map(([sent]) => sent)).toEqual([4, 8, 10]);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("a file that is an exact multiple of the part size has no empty last part", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-split-test-"));
  try {
    const path = join(temporary, "even.nsp");
    await writeFile(path, Buffer.alloc(8, 5));
    const { files, order, portal } = fakeConsole();
    await portal.pushFile("pkg", path, "LocalState/games", "even.nsp", undefined, 4);
    expect(order).toEqual(["even.nsp.nxpart000", "even.nsp.nxpart001", "even.nsp.nxparts"]);
    expect(files.get("even.nsp.nxparts")!.toString()).toBe("parts=2\nsize=8\n");
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("a file within the part size is one upload and no manifest", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-split-test-"));
  try {
    const path = join(temporary, "small.nsp");
    await writeFile(path, Buffer.alloc(4, 9));
    const { files, order, portal } = fakeConsole();
    await portal.pushFile("pkg", path, "LocalState/games", "base.nsp", undefined, 4);
    await portal.pushFile("pkg", path, "LocalState/games", "default.nsp");
    expect(order).toEqual(["base.nsp", "default.nsp"]);
    expect(files.get("base.nsp")!.length).toBe(4);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("a part that fails verification stops the upload before the manifest", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-split-test-"));
  try {
    const path = join(temporary, "big.nsp");
    await writeFile(path, Buffer.alloc(10, 3));
    const { order, portal } = fakeConsole("base.nsp.nxpart001");
    await expect(
      portal.pushFile("pkg", path, "LocalState/games", "base.nsp", undefined, 4),
    ).rejects.toThrow("portal");
    expect(order).toEqual(["base.nsp.nxpart000", "base.nsp.nxpart001"]);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("free space reads the selected package report and ignores missing, stale or invalid reports", async () => {
  const reports = [
    { free: 1234, total: 5678, updated: new Date().toISOString() },
    { free: 0, total: 5678, updated: new Date().toISOString() },
    { free: 1234, updated: new Date(Date.now() - 25 * 60 * 60_000).toISOString() },
    { free: 1234, updated: "invalid" },
    { free: -1, updated: new Date().toISOString() },
    { free: "1234", updated: new Date().toISOString() },
    {},
    null,
    "invalid json",
  ];
  for (const [index, report] of reports.entries()) {
    const portal = new DevicePortal("https://xbox.test", { user: "", password: "" }, true, (async (
      input: string | URL | Request,
    ) => {
      const url = new URL(String(input));
      expect(url.pathname).toBe("/api/filesystem/apps/file");
      expect(url.searchParams.get("packagefullname")).toBe("chosen-package");
      expect(url.searchParams.get("path")).toBe("\\LocalState");
      expect(url.searchParams.get("filename")).toBe("storage.json");
      if (report === null) return new Response("", { status: 404 });
      return typeof report === "string" ? new Response(report) : Response.json(report);
    }) as typeof fetch);
    expect(await portal.freeSpace("chosen-package")).toBe(
      index === 0 ? 1234 : index === 1 ? 0 : undefined,
    );
  }
});

test("HTTP 500 uploads report consoleFull unless the complete file is listed", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxhelper-full-test-"));
  try {
    const path = join(temporary, "game.nsp");
    await writeFile(path, "dump");
    for (const status of [500, 403]) {
      for (const size of [undefined, 2, 4]) {
        const mutations: string[] = [];
        const portal = new DevicePortal(
          "https://xbox.test",
          { user: "", password: "" },
          true,
          (async (input: string | URL | Request, init?: RequestInit) => {
            const url = new URL(String(input));
            if (url.pathname.endsWith("machinename"))
              return Response.json({}, { headers: { "Set-Cookie": "CSRF-Token=token" } });
            if (init?.method !== "GET") {
              mutations.push(init!.method!);
              await new Response(init?.body).arrayBuffer();
              return new Response("", { status });
            }
            return Response.json({
              Items: size === undefined ? [] : [{ Name: "game.nsp", FileSize: size }],
            });
          }) as typeof fetch,
        );
        const upload = portal.pushFile("pkg", path, "LocalState/games");
        if (status === 500 && size === 4) await upload;
        else
          await expect(upload).rejects.toMatchObject({
            code: status === 500 ? "consoleFull" : "portal",
          });
        expect(mutations).toEqual(["POST"]);
      }
    }
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
