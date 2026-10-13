import { describe, expect, test } from "bun:test";
import { XMLParser } from "fast-xml-parser";
import { detectLanguage, strings } from "../src/i18n";
import {
  draggedPath,
  filenameName,
  gameExtension,
  parseTitleId,
  validateFile,
  validateTitleId,
} from "../src/files";
import { generateManifest, packageVersion } from "../src/manifest";
import { parseArgs } from "../src/cli";
import { localCandidates } from "../src/discovery";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { tmpdir } from "node:os";
const titleId = "01007EF00011E000";

describe("system language", () => {
  test("Portuguese locale variants and environment precedence", () => {
    for (const locale of ["pt", "pt-BR", "PT_pt.UTF-8", "pt_PT@euro"])
      expect(detectLanguage({}, locale)).toBe("pt");
    expect(detectLanguage({ LC_ALL: "en_US", LANG: "pt_BR" }, "pt-BR")).toBe("en");
    expect(detectLanguage({ LC_ALL: "", LC_MESSAGES: "pt_PT", LANG: "en_US" })).toBe("pt");
    for (const locale of ["en", "de-DE", "C", "posix", "ptx"])
      expect(detectLanguage({}, locale)).toBe("en");
  });
  test("test overrides and equal translation keys", () => {
    expect(detectLanguage({ NXBOX_PACK_LANG: "en", LC_ALL: "pt_BR" })).toBe("en");
    expect(detectLanguage({ NXBOX_PACK_LANG: "pt", LC_ALL: "C" })).toBe("pt");
    expect(detectLanguage({ NXBOX_PACK_LANG: "xx", LANG: "pt_BR" })).toBe("pt");
    expect(Object.keys(strings.pt).sort()).toEqual(Object.keys(strings.en).sort());
  });
});
describe("local inputs", () => {
  test("only supported extensions, case insensitive", () => {
    for (const extension of ["nsp", "xci", "nsz", "xcz"] as const)
      expect(gameExtension(`Game.${extension.toUpperCase()}`)).toBe(`.${extension}`);
    for (const path of ["game.zip", "game.nsp.exe", "game", "game.nca"])
      expect(() => gameExtension(path)).toThrow();
  });
  test("dragged paths never execute shell expressions", () => {
    expect(draggedPath('"/tmp/Game [title].nsp"')).toBe(resolve("/tmp/Game [title].nsp"));
    if (process.platform !== "win32")
      expect(draggedPath("/tmp/Game\\ File.nsp")).toBe("/tmp/Game File.nsp");
    const filePath = join(tmpdir(), "Game File.nsp");
    expect(draggedPath(pathToFileURL(filePath).href)).toBe(filePath);
    expect(() => draggedPath("a\nb")).toThrow();
    expect(draggedPath("$(touch harmless).nsp")).toContain("$(touch harmless).nsp");
  });
  test("missing, empty and directory inputs are rejected", async () => {
    const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-test-"));
    try {
      const game = join(temporary, "game.nsp");
      await expect(validateFile(game)).rejects.toThrow();
      await writeFile(game, "");
      await expect(validateFile(game)).rejects.toThrow();
      await writeFile(game, "dump");
      await validateFile(game);
      const keys = join(temporary, "prod.keys");
      await writeFile(keys, "key = abc");
      await validateFile(keys, true);
      await expect(validateFile(game, true)).rejects.toThrow();
      await expect(validateFile(temporary)).rejects.toThrow();
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  });
  test("title IDs require one bracketed base game ID", () => {
    expect(parseTitleId(`Game [${titleId.toLowerCase()}][v0].nsp`)).toBe(titleId);
    expect(parseTitleId(`Game [${titleId}][${titleId}].nsp`)).toBeUndefined();
    expect(parseTitleId("Game [01007EF00011E800].nsp")).toBeUndefined();
    expect(parseTitleId(`${titleId}.nsp`)).toBeUndefined();
    expect(parseTitleId("Game [01007EF00011E0000].nsp")).toBeUndefined();
    expect(() => validateTitleId("../../bad")).toThrow();
    expect(filenameName(`My Game [${titleId}] [v0].xci`)).toBe("My Game");
  });
});
describe("independent game manifest", () => {
  test("retains runtime contracts and escapes title text", () => {
    const xml = generateManifest(titleId, 'Game & "Friends" <Deluxe>');
    const data = new XMLParser({ ignoreAttributes: false }).parse(xml).Package;
    expect(data.Identity["@_Name"]).toBe(`NSPX.NXbox.${titleId}`);
    expect(data.Identity["@_ProcessorArchitecture"]).toBe("x64");
    expect(data.Properties.DisplayName).toBe('Game & "Friends" <Deluxe>');
    expect(data.Applications.Application["@_Executable"]).toBe("nxbox.exe");
    expect(data.Applications.Application["@_EntryPoint"]).toBe("NXbox.App");
    expect(data.Applications.Application.Extensions).toBeUndefined();
    expect(xml).toContain("Microsoft.VCLibs.140.00");
    expect(xml).toContain("codeGeneration");
    expect(xml).not.toContain("windows.protocol");
  });
  test("rejects invalid names, title IDs and versions", () => {
    for (const name of ["", "A\0B", "a".repeat(65)])
      expect(() => generateManifest(titleId, name)).toThrow();
    for (const version of ["1.0.0", "1.0.65536.0", "a.0.0.0"])
      expect(() => generateManifest(titleId, "Game", version)).toThrow();
  });
  test("each pack upgrades the last one in place", () => {
    const order = (v: string) => v.split(".").reduce((a, n) => a * 65536 + Number(n), 0);
    const a = packageVersion(new Date("2026-10-13T00:00:00Z"));
    const b = packageVersion(new Date("2026-10-13T00:00:02Z"));
    const c = packageVersion(new Date("2026-10-14T00:00:00Z"));
    expect(order(a)).toBeGreaterThan(order("0.1.0.0"));
    expect(order(b)).toBeGreaterThan(order(a));
    expect(order(c)).toBeGreaterThan(order(b));
    expect(() =>
      generateManifest(titleId, "Game", packageVersion(new Date("2199-12-31T23:59:59Z"))),
    ).not.toThrow();
  });
});
test("download URL and checksum must be supplied together", () => {
  expect(() => parseArgs(["--runtime-url", "https://example.com/game.appx"])).toThrow();
  expect(() => parseArgs(["--language", "pt"])).toThrow();
  expect(() =>
    parseArgs(["--runtime-url", "http://example.com/a", "--runtime-sha256", "f".repeat(64)]),
  ).toThrow();
  expect(parseArgs(["--help"]).help).toBe(true);
});
test("discovery is bounded to private LAN interfaces", () => {
  const candidates = localCandidates({
    lan: [
      {
        address: "192.168.1.8",
        family: "IPv4",
        internal: false,
        netmask: "255.255.255.0",
        mac: "00:00:00:00:00:00",
        cidr: "192.168.1.8/24",
      },
    ],
  });
  expect(candidates).toHaveLength(253);
  expect(candidates).not.toContain("192.168.1.8");
  expect(candidates).toContain("192.168.1.9");
});
