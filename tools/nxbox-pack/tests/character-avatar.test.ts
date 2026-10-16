import { expect, test } from "bun:test";
import {
  avatarCharacters,
  avatarColors,
  characterAvatar,
  compositeAvatar,
} from "../src/character-avatar";
import { parseProfileArgs } from "../src/profile";

test("original catalogue has at least 24 unique characters and 12 named colors", () => {
  expect(avatarCharacters.length).toBeGreaterThanOrEqual(24);
  expect(new Set(avatarCharacters).size).toBe(avatarCharacters.length);
  expect(avatarColors.length).toBe(12);
  expect(new Set(avatarColors.map((c) => c.name)).size).toBe(12);
});
test("premultiplied fixture composites alpha over the selected background", () => {
  const fixture = new Uint8Array([100, 50, 25, 128, 0, 0, 0, 0, 200, 100, 50, 255]);
  expect(Array.from(compositeAvatar(fixture, [20, 40, 80]))).toEqual([
    110, 70, 65, 255, 20, 40, 80, 255, 200, 100, 50, 255,
  ]);
  expect(fixture[3]).toBe(128);
});
test("character command accepts either flag order and rejects unknown or repeated options", () => {
  const command = {
    kind: "character",
    name: "João",
    character: "fox-pilot",
    color: "blue",
  } as const;
  expect(
    parseProfileArgs(["avatar", "João", "--character", "fox-pilot", "--color", "blue"]).command,
  ).toEqual(command);
  expect(
    parseProfileArgs(["avatar", "João", "--color", "blue", "--character", "fox-pilot"]).command,
  ).toEqual(command);
  for (const options of [
    ["--character", "unknown", "--color", "blue"],
    ["--character", "fox-pilot", "--color", "unknown"],
    ["--color", "blue", "--color", "red"],
    ["--character", "fox-pilot"],
  ])
    expect(() => parseProfileArgs(["avatar", "João", ...options])).toThrow();
});
test("every character produces a bounded 256px JPEG without a browser or native decoder", () => {
  for (const id of avatarCharacters) {
    const jpeg = characterAvatar(id, "sky-blue");
    expect(jpeg.length).toBeLessThanOrEqual(0x20000);
    expect(Array.from(jpeg.slice(0, 2))).toEqual([255, 216]);
    expect(Array.from(jpeg.slice(-2))).toEqual([255, 217]);
    const sof = Buffer.from(jpeg).indexOf(Buffer.from([255, 192]));
    expect(Array.from(jpeg.slice(sof + 4, sof + 10))).toEqual([8, 1, 0, 1, 0, 3]);
  }
  expect(() => characterAvatar("../bad", "blue")).toThrow();
});
