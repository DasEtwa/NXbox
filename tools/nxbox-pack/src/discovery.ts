// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { networkInterfaces } from "node:os";
import { connect } from "node:net";
import { readFile } from "node:fs/promises";
import { homedir } from "node:os";
import { join } from "node:path";

async function probe(host: string): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = connect({ host, port: 11443 });
    const finish = (found: boolean) => {
      socket.destroy();
      resolve(found);
    };
    socket.setTimeout(500);
    socket.once("connect", () => finish(true));
    socket.once("error", () => finish(false));
    socket.once("timeout", () => finish(false));
  });
}
export function localCandidates(interfaces = networkInterfaces()): string[] {
  const addresses = new Set<string>();
  for (const entries of Object.values(interfaces))
    for (const entry of entries ?? []) {
      if (entry.internal || entry.family !== "IPv4") continue;
      const own = entry.address.split(".").map(Number);
      if (
        !(
          own[0] === 10 ||
          (own[0] === 192 && own[1] === 168) ||
          (own[0] === 172 && own[1]! >= 16 && own[1]! <= 31)
        )
      )
        continue;
      const prefix = own.slice(0, 3).join(".");
      for (let i = 1; i < 255; i++) if (i !== own[3]) addresses.add(`${prefix}.${i}`);
    }
  return [...addresses];
}
export async function discoverXbox(): Promise<string[]> {
  let cached = "";
  try {
    cached = (await readFile(join(homedir(), ".local/share/nxbox/host.txt"), "utf8")).trim();
  } catch {
    /* Optional existing XboxDev hint. */
  }
  const hints = [cached, "xbox.local"].filter(Boolean);
  const hits = (
    await Promise.all(hints.map(async (host) => ((await probe(host)) ? host : "")))
  ).filter(Boolean);
  if (hits.length) return [...new Set(hits)];
  const candidates = localCandidates().slice(0, 1024);
  let next = 0;
  await Promise.all(
    Array.from({ length: 24 }, async () => {
      while (next < candidates.length) {
        const host = candidates[next++]!;
        if (await probe(host)) hits.push(host);
      }
    }),
  );
  return hits;
}
