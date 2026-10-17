// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { avatarCharacters, avatarColors } from "./character-avatar";
import { join } from "node:path";
import { PackError } from "./i18n";
import type { DevicePortal } from "./portal";

export const avatarDirectory = "LocalState/eden/nand/system/save/8000000000000010/su/avators";
export type ProfileCommand =
  | { kind: "multi"; enabled: boolean }
  | { kind: "avatar"; name: string; image: string }
  | { kind: "character"; name: string; character: string; color: string };
export function parseProfileArgs(args: string[]): { command: ProfileCommand; insecure: boolean } {
  const insecure = args.includes("--insecure-portal");
  args = args.filter((arg) => arg !== "--insecure-portal");
  if (args.length === 2 && args[0] === "multi" && /^(on|off)$/.test(args[1]!))
    return { command: { kind: "multi", enabled: args[1] === "on" }, insecure };
  if (args.length === 3 && args[0] === "avatar" && args[1] && args[2])
    return { command: { kind: "avatar", name: args[1], image: args[2] }, insecure };
  if (args.length === 6 && args[0] === "avatar" && args[1]) {
    const options = new Map([
      [args[2], args[3]],
      [args[4], args[5]],
    ]);
    const character = options.get("--character");
    const color = options.get("--color");
    if (
      character &&
      color &&
      avatarCharacters.includes(character) &&
      avatarColors.some(({ name }) => name === color)
    )
      return { command: { kind: "character", name: args[1], character, color }, insecure };
  }
  throw new PackError("profileUsage");
}

export function parseProfiles(bytes: Uint8Array): { name: string; uuid: string }[] {
  // ProfileDataRaw: 16-byte header, eight 0xC8-byte UserRaw records. UUID bytes are
  // Common::UUID's byte order; the save folder's U128 order must never be used for avatars.
  if (bytes.length !== 0x650) throw new PackError("profileMissing");
  const profiles = [];
  for (let i = 0; i < 8; ++i) {
    const base = 0x10 + i * 0xc8;
    const uuid = Buffer.from(bytes.subarray(base, base + 16)).toString("hex");
    if (/^0+$/.test(uuid)) continue;
    const field = bytes.subarray(base + 0x28, base + 0x48);
    const zero = field.indexOf(0);
    const name = new TextDecoder("utf-8", { fatal: true }).decode(
      zero < 0 ? field : field.subarray(0, zero),
    );
    profiles.push({ name, uuid });
  }
  return profiles;
}
export function avatarFilename(uuid: string): string {
  if (!/^[a-f0-9]{32}$/i.test(uuid) || /^0+$/.test(uuid)) throw new PackError("profileMissing");
  uuid = uuid.toLowerCase();
  return `${uuid.slice(0, 8)}-${uuid.slice(8, 12)}-${uuid.slice(12, 16)}-${uuid.slice(16, 20)}-${uuid.slice(20)}.jpg`;
}

export async function setMulti(
  portal: DevicePortal,
  pkg: string,
  enabled: boolean,
  temporary: string,
): Promise<void> {
  let state: Record<string, unknown> = {};
  // Missing is different from an unreadable or malformed file. Never silently discard state.
  if ((await portal.listFiles(pkg, "LocalState")).some((file) => file.Name === "accounts.json")) {
    const value = JSON.parse(
      new TextDecoder().decode(await portal.readFile(pkg, "LocalState", "accounts.json")),
    );
    if (!value || typeof value !== "object" || Array.isArray(value))
      throw new PackError("profileMissing");
    state = value;
  }
  state = { ...state, initialized: true, multi: enabled };
  const file = join(temporary, "accounts.json");
  await Bun.write(file, JSON.stringify(state, null, 2));
  await portal.pushFile(pkg, file, "LocalState", "accounts.json");
}

export async function uploadAvatar(
  portal: DevicePortal,
  pkg: string,
  name: string,
  jpeg: Uint8Array,
  temporary: string,
): Promise<void> {
  const profiles = parseProfiles(await portal.readFile(pkg, avatarDirectory, "profiles.dat"));
  const matches = profiles.filter((profile) => profile.name === name);
  if (matches.length !== 1) throw new PackError("profileMissing");
  const remote = avatarFilename(matches[0]!.uuid);
  const file = join(temporary, remote);
  await Bun.write(file, jpeg);
  await portal.pushFile(pkg, file, avatarDirectory, remote);
}
