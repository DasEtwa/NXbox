import { basename, extname, join } from "node:path";
import { mkdir } from "node:fs/promises";
import { gameExtension, validateFile } from "./files";
import { PackError } from "./i18n";
export async function prepareContent(
  path: string,
  directory: string,
  keys: string,
): Promise<string> {
  const extension = gameExtension(path);
  if (extension !== ".nsz" && extension !== ".xcz") return path;
  if (!Bun.which("nsz")) throw new PackError("compressed");
  await mkdir(directory, { recursive: true, mode: 0o700 });
  const child = Bun.spawn(["nsz", "-D", "--keys", keys, "-o", directory, path], {
    cwd: directory,
    stdin: "ignore",
    stdout: "ignore",
    stderr: "ignore",
  });
  if ((await child.exited) !== 0) throw new PackError("compressed");
  const output = join(
    directory,
    basename(path, extname(path)) + (extension === ".nsz" ? ".nsp" : ".xci"),
  );
  await validateFile(output);
  return output;
}
