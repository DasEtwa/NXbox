import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { gameExtension } from "./files";
import { packageIdentity } from "./manifest";
import type { DevicePortal } from "./portal";
export async function deployGame(options: {
  portal: DevicePortal;
  titleId: string;
  runtimeDirectory: string;
  runtimeFiles: string[];
  content: string[];
  keys: string;
  temporary: string;
  onUploading?: () => void;
  onProgress?: (name: string, sent: number, total: number) => void;
}): Promise<string> {
  const { portal, titleId, runtimeDirectory, runtimeFiles, content, keys, temporary } = options;
  if (!content.length) throw new Error("A base game is required");
  const identity = packageIdentity(titleId);
  const folder = `${identity}-${crypto.randomUUID()}`;
  for (const file of runtimeFiles)
    await portal.uploadRuntime(folder, join(runtimeDirectory, file), file);
  const pkg = await portal.register(folder, identity);
  options.onUploading?.();
  await portal.makeFolder(pkg.PackageFullName, "LocalState", "games");
  await portal.makeFolder(pkg.PackageFullName, "LocalState", "eden");
  await portal.makeFolder(pkg.PackageFullName, "LocalState/eden", "keys");
  await portal.pushFile(pkg.PackageFullName, keys, "LocalState/eden/keys", "prod.keys");
  for (const [index, path] of content.entries()) {
    const remoteName = `${index === 0 ? "base" : `addon-${index}`}${gameExtension(path)}`;
    await portal.pushFile(
      pkg.PackageFullName,
      path,
      "LocalState/games",
      remoteName,
      (sent, total) => options.onProgress?.(remoteName, sent, total),
    );
  }
  // Activate direct boot only after all content transfers have been verified.
  for (const [file, value] of [
    ["game.txt", `games\\base${gameExtension(content[0]!)}\n`],
    ["skip_library.txt", "1\n"],
  ] as const) {
    const path = join(temporary, file);
    await writeFile(path, value, { mode: 0o600 });
    await portal.pushFile(pkg.PackageFullName, path, "LocalState");
  }
  return identity;
}
