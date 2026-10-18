// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { basename, extname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { PackError, strings, type Language } from "./i18n";

export function draggedPath(input: string): string {
  let value = input.trim();
  if (
    (value.startsWith('"') && value.endsWith('"')) ||
    (value.startsWith("'") && value.endsWith("'"))
  )
    value = value.slice(1, -1);
  if (value.startsWith("file://")) return fileURLToPath(value);
  // Terminal drag-and-drop escapes spaces on Unix. Never evaluate shell input.
  if (process.platform !== "win32") value = value.replace(/\\([\s()\[\]'"&])/g, "$1");
  if (!value || /[\r\n\0]/.test(value)) throw new PackError("file");
  return resolve(value);
}
export function gameExtension(path: string): ".nsp" | ".xci" | ".nsz" | ".xcz" {
  const extension = extname(path).toLowerCase();
  if (![".nsp", ".xci", ".nsz", ".xcz"].includes(extension)) throw new PackError("file");
  return extension as ReturnType<typeof gameExtension>;
}
export async function validateFile(path: string, keys = false): Promise<void> {
  if (keys ? basename(path).toLowerCase() !== "prod.keys" : !gameExtension(path))
    throw new PackError(keys ? "keyFile" : "file");
  const file = Bun.file(path);
  try {
    if (
      !(await file.exists()) ||
      file.size === 0 ||
      !(await (await import("node:fs/promises")).stat(path)).isFile()
    )
      throw new Error("Invalid file");
  } catch (cause) {
    throw new PackError(keys ? "keyFile" : "file", { cause });
  }
}
export function validateTitleId(value: string): string {
  if (!/^0100[0-9a-f]{9}000$/i.test(value)) throw new PackError("titleInvalid");
  return value.toUpperCase();
}
export function parseTitleId(path: string): string | undefined {
  const matches = [...basename(path).matchAll(/\[(0100[0-9a-f]{12})\]/gi)];
  if (matches.length !== 1) return undefined;
  try {
    return validateTitleId(matches[0]![1]!);
  } catch {
    return undefined;
  }
}
export function filenameName(path: string): string {
  return basename(path, extname(path))
    .replace(/\[[^\]]*\]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 64);
}
export function validateName(name: string): string {
  const value = name.trim();
  if (!value || value.length > 64 || /[\x00-\x1f\x7f]/.test(value))
    throw new PackError("nameInvalid");
  return value;
}

export type GameFolder = "Base" | "Updates" | "DLC" | "";
export function titleFolder(titleId: string | undefined): GameFolder {
  if (!titleId || !/^0100[0-9a-f]{12}$/i.test(titleId)) return "";
  const suffix = Number(BigInt(`0x${titleId}`) & 0xfffn);
  if (suffix === 0) return "Base";
  if (suffix === 0x800) return "Updates";
  if (suffix > 0 && suffix < 0x800) return "DLC";
  return "";
}
export async function gameDestination(path: string, keysPath?: string): Promise<string> {
  let folder: GameFolder = "";
  if (keysPath) {
    const { packageEntries } = await import("./metadata");
    const { fileReader } = await import("./storage");
    const { readKeys, ncaHeader } = await import("./firmware");
    let reader: Awaited<ReturnType<typeof fileReader>> | undefined;
    try {
      const key = (await readKeys(keysPath)).get("header_key");
      if (key?.length === 32) {
        reader = await fileReader([path]);
        const entries = await packageEntries(reader, path);
        const kinds = new Set<GameFolder>();
        for (const entry of entries.filter((e) => /\.nca$/i.test(e.name))) {
          try {
            const header = ncaHeader(await entry.reader.read(0, 0xc00), key);
            if (header[0x205] !== 1) continue; // Meta NCAs identify updates and DLC too.
            kinds.add(titleFolder(header.readBigUInt64LE(0x210).toString(16).padStart(16, "0")));
          } catch {
            /* Keep inspecting the remaining metadata NCAs. */
          }
        }
        // Bundles containing a base game belong with base games.
        folder = kinds.has("Base") ? "Base" : kinds.size === 1 ? [...kinds][0]! : "";
      }
    } catch {
      /* Unreadable metadata retains the filename fallback. */
    } finally {
      await reader?.close();
    }
  }
  if (!folder) {
    const ids = [...basename(path).matchAll(/\[(0100[0-9a-f]{12})\]/gi)];
    if (ids.length === 1) folder = titleFolder(ids[0]![1]);
  }
  return [folder, basename(path)].filter(Boolean).join("/");
}

export function destinationProgress(path: string, language: Language = "en") {
  const parts = path.replaceAll("\\", "/").split("/");
  return strings[language].gameDestination
    .replace("{file}", parts.pop()!)
    .replace("{folder}", parts.join("/"));
}
