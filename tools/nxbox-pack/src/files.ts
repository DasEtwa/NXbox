import { basename, extname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { PackError } from "./i18n";

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
