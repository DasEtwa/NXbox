import { expect, test } from "bun:test";
import { zipSync } from "fflate";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { downloadRuntime, runtimePayload, safeArchivePath, stageRuntime } from "../src/runtime";
export function fixture(): Record<string, Uint8Array> {
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
      "Assets/avatars/fox-pilot.png",
      "Notices/Eden-LICENSE.txt",
      "Notices/Mesa-LICENSE.rst",
      "Notices/DXC-LICENSE-MS.txt",
      "Notices/DXC-LICENSE-LLVM.txt",
    ].map((name) => [
      name,
      name === "nxbox.exe" ? executable : new TextEncoder().encode("fixture"),
    ]),
  );
}
test("stages only runtime payload, never private or stale boot data", async () => {
  const packageData = zipSync({
    ...fixture(),
    "prod.keys": new Uint8Array([1]),
    "games/private.nsp": new Uint8Array([1]),
    "skip_library.txt": new Uint8Array([1]),
    "AppxSignature.p7x": new Uint8Array([1]),
  });
  const artifact = zipSync({ "NXbox_game_0.3.1.0_x64.appx": packageData });
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-runtime-test-"));
  try {
    const files = await stageRuntime(artifact, temporary, "01007EF00011E000", "My Game");
    expect(files).not.toContain("prod.keys");
    expect(files).not.toContain("games/private.nsp");
    // game.nro is the runtime's own marker that boots the game session, so it must ship.
    expect(files).toContain("game.nro");
    expect(files).toContain("Assets/avatars/fox-pilot.png");
    expect(await readFile(join(temporary, "Assets/avatars/fox-pilot.png"), "utf8")).toBe("fixture");
    expect(files).not.toContain("AppxSignature.p7x");
    expect(await readFile(join(temporary, "AppxManifest.xml"), "utf8")).toContain(
      'DisplayName="My Game"',
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
test("rejects traversal, incomplete runtimes and invalid PE architectures", () => {
  for (const name of ["../keys", "/absolute", "C:/file", "Assets\\file", "a/../file"])
    expect(() => safeArchivePath(name)).toThrow();
  expect(() => runtimePayload(zipSync({ "../escape": new Uint8Array([1]) }))).toThrow();
  expect(() => runtimePayload(zipSync({ "nxbox.exe": new Uint8Array([1]) }))).toThrow();
  const files = fixture();
  files["nxbox.exe"]![68] = 0;
  expect(() => runtimePayload(zipSync(files))).toThrow();
});
test("runtime downloads are checked against SHA-256", async () => {
  const data = zipSync(fixture());
  const original = globalThis.fetch;
  globalThis.fetch = (async () => new Response(Buffer.from(data))) as unknown as typeof fetch;
  try {
    const source = {
      url: "https://example.test/runtime.appx",
      sha256: createHash("sha256").update(data).digest("hex"),
    };
    expect(await downloadRuntime(source)).toEqual(data);
    await expect(downloadRuntime({ ...source, sha256: "0".repeat(64) })).rejects.toThrow();
  } finally {
    globalThis.fetch = original;
  }
});
