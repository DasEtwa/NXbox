// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { createHash } from "node:crypto";
import { mkdir, writeFile } from "node:fs/promises";
import { join, posix } from "node:path";
import { unzipSync } from "fflate";
import { PackError } from "./i18n";
import { generateManifest } from "./manifest";

export type RuntimeSource = { url: string; sha256: string };
const repository = "NspxMiguel/NXbox";
const archiveLimit = 256 * 1024 * 1024;
const expandedLimit = 768 * 1024 * 1024;
export function validateSource(source: RuntimeSource): RuntimeSource {
  const url = new URL(source.url);
  if (
    url.protocol !== "https:" ||
    url.username ||
    url.password ||
    !/^[a-f0-9]{64}$/i.test(source.sha256)
  )
    throw new PackError("runtimeInvalid");
  return source;
}
async function github(path: string): Promise<any> {
  const token =
    process.env.NXBHELPER_GITHUB_TOKEN ??
    process.env.NXHELPER_GITHUB_TOKEN ??
    process.env.NXBOX_PACK_GITHUB_TOKEN;
  const response = await fetch(`https://api.github.com/repos/${repository}/${path}`, {
    headers: {
      Accept: "application/vnd.github+json",
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
    },
    signal: AbortSignal.timeout(30_000),
  });
  if (!response.ok) throw new PackError("runtimeMissing");
  return response.json();
}
export async function latestRuntime(): Promise<RuntimeSource> {
  try {
    const release = await github("releases/latest");
    const asset = release.assets?.find(
      (a: any) =>
        /^NXbox_game_.*\.appx$/i.test(a.name) && /^sha256:[a-f0-9]{64}$/i.test(a.digest ?? ""),
    );
    if (asset)
      return validateSource({ url: asset.browser_download_url, sha256: asset.digest.slice(7) });
  } catch {
    /* A repository may only have CI artifacts yet. */
  }
  try {
    const runs = await github(
      "actions/workflows/package-nxbox.yml/runs?branch=main&status=success&per_page=5",
    );
    for (const run of runs.workflow_runs ?? []) {
      if (
        run.head_branch !== "main" ||
        run.conclusion !== "success" ||
        run.head_repository?.full_name !== repository ||
        run.event === "pull_request"
      )
        continue;
      const data = await github(`actions/runs/${run.id}/artifacts`);
      const artifact = data.artifacts?.find(
        (a: any) =>
          /^(nxbox-release|nxbox-diagnostic)$/.test(a.name) &&
          !a.expired &&
          /^sha256:[a-f0-9]{64}$/i.test(a.digest ?? ""),
      );
      if (artifact)
        return validateSource({
          url: artifact.archive_download_url,
          sha256: artifact.digest.slice(7),
        });
    }
  } catch {
    /* Explicit URL/checksum remains available. */
  }
  throw new PackError("runtimeMissing");
}
export async function downloadRuntime(source: RuntimeSource): Promise<Uint8Array> {
  validateSource(source);
  const url = new URL(source.url);
  const token =
    process.env.NXBHELPER_GITHUB_TOKEN ??
    process.env.NXHELPER_GITHUB_TOKEN ??
    process.env.NXBOX_PACK_GITHUB_TOKEN;
  const response = await fetch(url, {
    headers:
      token && url.origin === "https://api.github.com"
        ? { Authorization: `Bearer ${token}`, Accept: "application/vnd.github+json" }
        : {},
    signal: AbortSignal.timeout(300_000),
  });
  if (url.origin === "https://api.github.com" && [401, 403].includes(response.status))
    throw new PackError("runtimeMissing");
  if (
    !response.ok ||
    !response.body ||
    Number(response.headers.get("content-length")) > archiveLimit
  )
    throw new PackError("runtimeInvalid");
  const chunks: Uint8Array[] = [];
  const hash = createHash("sha256");
  let size = 0;
  for await (const chunk of response.body) {
    size += chunk.length;
    if (size > archiveLimit) throw new PackError("runtimeInvalid");
    hash.update(chunk);
    chunks.push(chunk);
  }
  if (hash.digest("hex").toLowerCase() !== source.sha256.toLowerCase())
    throw new PackError("runtimeInvalid");
  return Buffer.concat(chunks);
}
export function safeArchivePath(name: string): string {
  if (
    !name ||
    /[\\:\x00-\x1f]/.test(name) ||
    name.startsWith("/") ||
    name.split("/").some((p) => p === "." || p === "..")
  )
    throw new PackError("runtimeInvalid");
  return name;
}
function unpack(data: Uint8Array): Record<string, Uint8Array> {
  let expanded = 0;
  return unzipSync(data, {
    filter: (entry) => {
      safeArchivePath(entry.name);
      expanded += entry.originalSize;
      if (expanded > expandedLimit) throw new PackError("runtimeInvalid");
      return !entry.name.endsWith("/");
    },
  });
}
export function runtimePayload(data: Uint8Array): Record<string, Uint8Array> {
  let entries = unpack(data);
  const packages = Object.keys(entries).filter((n) =>
    /^NXbox_game_.*\.appx$/i.test(posix.basename(n)),
  );
  if (packages.length) {
    if (packages.length !== 1) throw new PackError("runtimeInvalid");
    entries = unpack(entries[packages[0]!]!);
  }
  // Do not copy signed package metadata, homebrew, configs, keys or game dumps.
  const payload = Object.fromEntries(
    Object.entries(entries).filter(
      ([n]) =>
        /^[^/]+\.(exe|dll)$/i.test(n) ||
        // The bundled game.nro marker switches nxbox.exe into the game session (game.txt).
        n === "game.nro" ||
        /^Assets\/(?:avatars\/)?[^/]+\.png$/i.test(n) ||
        /^Notices\/[^/]+\.(txt|md|rst)$/i.test(n),
    ),
  );
  for (const name of [
    "nxbox.exe",
    "game.nro",
    "opengl32.dll",
    "libgallium_wgl.dll",
    "libglapi.dll",
    "dxil.dll",
    "Assets/StoreLogo.png",
    "Assets/Square44x44Logo.png",
    "Assets/Square150x150Logo.png",
    "Notices/Eden-LICENSE.txt",
    "Notices/Mesa-LICENSE.rst",
    "Notices/DXC-LICENSE-MS.txt",
    "Notices/DXC-LICENSE-LLVM.txt",
  ]) {
    if (!payload[name]?.length) throw new PackError("runtimeInvalid");
  }
  const exe = Buffer.from(payload["nxbox.exe"]!);
  const offset = exe.length >= 64 ? exe.readUInt32LE(0x3c) : exe.length;
  if (
    exe.toString("ascii", 0, 2) !== "MZ" ||
    offset + 6 > exe.length ||
    !exe.subarray(offset, offset + 6).equals(Buffer.from([80, 69, 0, 0, 100, 134]))
  )
    throw new PackError("runtimeInvalid");
  return payload;
}
export async function stageRuntime(
  data: Uint8Array,
  directory: string,
  titleId: string,
  name: string,
  /** The game's own JPEG icon (from its control data) becomes the dashboard tile. */
  icon?: Uint8Array,
): Promise<string[]> {
  try {
    const payload = runtimePayload(data);
    const logo = icon?.[0] === 0xff && icon[1] === 0xd8 ? "Assets\\GameIcon.jpg" : undefined;
    if (logo) payload["Assets/GameIcon.jpg"] = icon!;
    payload["AppxManifest.xml"] = new TextEncoder().encode(
      generateManifest(titleId, name, undefined, logo),
    );
    for (const [path, content] of Object.entries(payload)) {
      await mkdir(join(directory, posix.dirname(path)), { recursive: true, mode: 0o700 });
      await writeFile(join(directory, path), content, { mode: 0o600 });
    }
    return Object.keys(payload);
  } catch (cause) {
    throw new PackError("runtimeInvalid", { cause });
  }
}
