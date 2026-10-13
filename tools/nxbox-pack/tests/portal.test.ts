import { expect, test } from "bun:test";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { DevicePortal, portalUrl } from "../src/portal";

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
    const runtime = ["nxbox.exe", "opengl32.dll", "Assets/logo.png", "Assets/wide.png"];
    await mkdir(join(temporary, "Assets"));
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
    ).toEqual(["game-folder", "game-folder\\Assets"]);
    expect(uploads[0]!.body).toContain('filename="nxbox.exe"');
    expect(uploads[0]!.body).toContain('filename="opengl32.dll"');
    expect(uploads[1]!.body).toContain('filename="logo.png"');
    expect(uploads[1]!.body).toContain('filename="wide.png"');
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
