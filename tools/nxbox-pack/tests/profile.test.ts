// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { DevicePortal } from "../src/portal";
import {
  avatarDirectory,
  avatarFilename,
  parseProfileArgs,
  parseProfiles,
  setMulti,
  uploadAvatar,
} from "../src/profile";
import { encodeAvatarJpeg } from "../src/jpeg";

function fixture(duplicate = false): Uint8Array {
  const bytes = new Uint8Array(0x650);
  bytes.set(Buffer.from("00112233445566778899aabbccddeeff", "hex"), 0x10);
  bytes.set(new TextEncoder().encode("João"), 0x38);
  if (duplicate) {
    bytes.set(bytes.slice(0x10, 0xd8), 0xd8);
    bytes[0xd8] = 0xaa;
  }
  return bytes;
}
test("profile command validates exact arguments", () => {
  expect(parseProfileArgs(["multi", "on"]).command).toEqual({ kind: "multi", enabled: true });
  expect(parseProfileArgs(["multi", "off"]).command).toEqual({ kind: "multi", enabled: false });
  expect(parseProfileArgs(["avatar", "João", "a.webp", "--insecure-portal"]).insecure).toBe(true);
  for (const args of [
    [],
    ["multi", "yes"],
    ["multi", "on", "extra"],
    ["avatar", "NXbox"],
    ["avatar", "", "a.jpg"],
  ])
    expect(() => parseProfileArgs(args)).toThrow();
});
test("real profile record layout and UUID avatar byte order", () => {
  expect(parseProfiles(fixture())).toEqual([
    { name: "João", uuid: "00112233445566778899aabbccddeeff" },
  ]);
  expect(avatarFilename(parseProfiles(fixture())[0]!.uuid)).toBe(
    "00112233-4455-6677-8899-aabbccddeeff.jpg",
  );
  expect(() => parseProfiles(new Uint8Array(100))).toThrow();
  expect(() => avatarFilename("../bad")).toThrow();
  expect(() => avatarFilename("0".repeat(32))).toThrow();
});

test("Device Portal round trip preserves account mode fields and uploads only matching UUID", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-profile-test-"));
  const uploads: { path: string; name: string; data: Uint8Array }[] = [];
  const previous = {
    initialized: true,
    multi: true,
    selected_uuid: "a".repeat(32),
    future_field: 42,
  };
  const transport = (async (input: URL | string | Request, init?: RequestInit) => {
    const url = new URL(String(input));
    if (url.pathname.endsWith("machinename"))
      return Response.json({}, { headers: { "Set-Cookie": "CSRF-Token=token" } });
    if (url.pathname.endsWith("/files"))
      return Response.json({
        Items: [
          ...uploads.map((file) => ({ Name: file.name, FileSize: file.data.length })),
          { Name: "accounts.json" },
        ],
      });
    if (init?.method === "POST") {
      const body = await new Response(init.body, {
        headers: { "Content-Type": new Headers(init.headers).get("Content-Type")! },
      }).formData();
      const file = body.get("file") as File;
      uploads.push({
        path: url.searchParams.get("path")!,
        name: file.name,
        data: new Uint8Array(await file.arrayBuffer()),
      });
      return new Response("");
    }
    expect(url.searchParams.get("packagefullname")).toBe("pkg");
    return url.searchParams.get("filename") === "profiles.dat"
      ? new Response(Buffer.from(fixture()))
      : Response.json(previous);
  }) as typeof fetch;
  const portal = new DevicePortal(
    "https://xbox.test:11443",
    { user: "", password: "" },
    true,
    transport,
  );
  try {
    await setMulti(portal, "pkg", false, temporary);
    expect(JSON.parse(new TextDecoder().decode(uploads[0]!.data))).toEqual({
      ...previous,
      multi: false,
    });
    expect(uploads[0]!.path).toBe("\\LocalState");
    const jpeg = encodeAvatarJpeg(new Uint8Array(256 * 256 * 4));
    await uploadAvatar(portal, "pkg", "João", jpeg, temporary);
    expect(uploads[1]!.path).toBe(`\\${avatarDirectory.replaceAll("/", "\\")}`);
    expect(uploads[1]!.name).toBe("00112233-4455-6677-8899-aabbccddeeff.jpg");
    expect(uploads[1]!.data).toEqual(jpeg);
    await expect(uploadAvatar(portal, "pkg", "missing", jpeg, temporary)).rejects.toThrow();
    expect(uploads.length).toBe(2);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
test("duplicate profile names remain ambiguous", () => {
  expect(parseProfiles(fixture(true)).filter((profile) => profile.name === "João").length).toBe(2);
});
test("pure TypeScript encoder writes a baseline 256x256 color JPEG", () => {
  const rgba = new Uint8Array(256 * 256 * 4);
  for (let i = 0; i < rgba.length; i += 4) {
    rgba[i] = 200;
    rgba[i + 1] = 80;
    rgba[i + 2] = 20;
    rgba[i + 3] = 255;
  }
  const jpeg = encodeAvatarJpeg(rgba);
  expect(Array.from(jpeg.slice(0, 2))).toEqual([255, 216]);
  expect(Array.from(jpeg.slice(-2))).toEqual([255, 217]);
  const sof = Buffer.from(jpeg).indexOf(Buffer.from([255, 192]));
  expect(Array.from(jpeg.slice(sof + 4, sof + 10))).toEqual([8, 1, 0, 1, 0, 3]);
  expect(() => encodeAvatarJpeg(new Uint8Array(4))).toThrow();
});

test("detailed avatars fit the guest's 128 KiB limit without truncation", () => {
  const rgba = new Uint8Array(256 * 256 * 4);
  let seed = 42;
  for (let i = 0; i < rgba.length; ++i) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    rgba[i] = i % 4 === 3 ? 255 : seed >>> 24;
  }
  const jpeg = encodeAvatarJpeg(rgba);
  expect(jpeg.length).toBeLessThanOrEqual(0x20000);
  expect(Array.from(jpeg.slice(-2))).toEqual([255, 217]);
});

test("mode setting preserves the legacy initialized field and refuses malformed existing state", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-mode-test-"));
  const writes: unknown[] = [];
  let exists = false;
  let contents = "{bad";
  const portal = {
    listFiles: async () => (exists ? [{ Name: "accounts.json" }] : []),
    readFile: async () => new TextEncoder().encode(contents),
    pushFile: async (_pkg: string, file: string) => {
      writes.push(await Bun.file(file).json());
    },
  } as unknown as DevicePortal;
  try {
    await setMulti(portal, "pkg", true, temporary);
    expect(writes).toEqual([{ initialized: true, multi: true }]);
    exists = true;
    for (contents of ["{bad", "null", "[]"]) {
      await expect(setMulti(portal, "pkg", false, temporary)).rejects.toThrow();
    }
    expect(writes.length).toBe(1);
    let uploaded = false;
    const duplicate = {
      readFile: async () => fixture(true),
      pushFile: async () => {
        uploaded = true;
      },
    } as unknown as DevicePortal;
    await expect(
      uploadAvatar(duplicate, "pkg", "João", new Uint8Array(), temporary),
    ).rejects.toThrow();
    expect(uploaded).toBe(false);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
