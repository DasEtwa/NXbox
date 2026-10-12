import { createReadStream } from "node:fs";
import { basename } from "node:path";
import { Readable } from "node:stream";
import { PackError } from "./i18n";

export type Credentials = { user: string; password: string };
export type InstalledPackage = {
  Name: string;
  PackageFullName: string;
  PackageRelativeId?: string;
};
export function portalUrl(host: string): string {
  const url = new URL(
    host.includes("://") ? host : `https://${host}${host.includes(":") ? "" : ":11443"}`,
  );
  if (
    url.protocol !== "https:" ||
    url.username ||
    url.password ||
    url.pathname !== "/" ||
    url.search ||
    url.hash
  )
    throw new PackError("portal");
  return url.origin;
}
export class DevicePortal {
  private csrf = "";
  constructor(
    readonly base: string,
    private readonly credentials: Credentials,
    private readonly insecure = false,
    private readonly transport: typeof fetch = fetch,
  ) {}
  private async raw(method: string, path: string, init: RequestInit = {}): Promise<Response> {
    const headers = new Headers(init.headers);
    headers.set(
      "Authorization",
      `Basic ${Buffer.from(`${this.credentials.user}:${this.credentials.password}`).toString("base64")}`,
    );
    if (this.csrf) {
      headers.set("Cookie", `CSRF-Token=${this.csrf}`);
      headers.set("X-CSRF-Token", this.csrf);
    }
    const response = await this.transport(`${this.base}${path}`, {
      ...init,
      method,
      headers,
      redirect: "manual",
      signal: init.signal ?? AbortSignal.timeout(60_000),
      tls: { rejectUnauthorized: !this.insecure },
    } as RequestInit);
    const cookie = response.headers.get("set-cookie")?.match(/CSRF-Token=([^;,\s]+)/i)?.[1];
    if (cookie) this.csrf = cookie;
    if (!response.ok)
      throw new PackError("portal", {
        cause: new Error(`${method} ${path.split("?")[0]}: HTTP ${response.status}`),
      });
    return response;
  }
  async request(method: string, path: string, init: RequestInit = {}): Promise<Response> {
    if (method !== "GET" && !this.csrf) await this.raw("GET", "/api/os/machinename");
    return this.raw(method, path, init);
  }
  async packages(): Promise<InstalledPackage[]> {
    const data = (await (await this.request("GET", "/api/app/packagemanager/packages")).json()) as {
      InstalledPackages?: InstalledPackage[];
    };
    return data.InstalledPackages ?? [];
  }
  async register(
    folder: string,
    identity: string,
    timeout = 15 * 60_000,
  ): Promise<InstalledPackage> {
    await this.request(
      "POST",
      `/api/app/packagemanager/register?${new URLSearchParams({ folder: Buffer.from(folder).toString("base64") })}`,
    );
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      await Bun.sleep(1000);
      const state = await this.request("GET", "/api/app/packagemanager/state");
      if (state.status === 204) continue;
      const text = await state.text();
      if (text.trim()) {
        const result = JSON.parse(text) as { Code?: number; Success?: boolean };
        if (result.Success === false || (result.Code !== undefined && Number(result.Code) !== 0))
          throw new PackError("deploy");
      }
      const installed = (await this.packages()).find((p) => p.Name === identity);
      if (installed) return installed;
    }
    throw new PackError("deploy");
  }
  private async sendFile(
    endpoint: string,
    path: string,
    remoteName: string,
    onProgress?: (bytes: number, total: number) => void,
  ): Promise<void> {
    // Stream multipart bodies with backpressure; never buffer multi-GB dumps in FormData.
    const file = Bun.file(path);
    if (!(await file.exists()) || /["\r\n\x00]/.test(remoteName)) throw new PackError("file");
    const boundary = `nxbox-${crypto.randomUUID()}`;
    const prefix = Buffer.from(
      `--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="${remoteName}"\r\nContent-Type: application/octet-stream\r\n\r\n`,
    );
    const suffix = Buffer.from(`\r\n--${boundary}--\r\n`);
    const total = file.size;
    async function* body() {
      yield prefix;
      let sent = 0;
      for await (const chunk of createReadStream(path, { highWaterMark: 1024 * 1024 })) {
        sent += chunk.length;
        onProgress?.(sent, total);
        yield chunk;
      }
      yield suffix;
    }
    await this.request("POST", endpoint, {
      headers: {
        "Content-Type": `multipart/form-data; boundary=${boundary}`,
        "Content-Length": String(prefix.length + total + suffix.length),
      },
      body: Readable.toWeb(Readable.from(body())) as unknown as ReadableStream,
      signal: AbortSignal.timeout(12 * 60 * 60_000),
    });
  }
  async uploadRuntime(folder: string, path: string, relativePath: string): Promise<void> {
    const parent = relativePath.includes("/")
      ? relativePath.slice(0, relativePath.lastIndexOf("/"))
      : "";
    const destination = [folder, parent].filter(Boolean).join("\\").replaceAll("/", "\\");
    await this.sendFile(
      `/api/app/packagemanager/upload?${new URLSearchParams({ destinationFolder: Buffer.from(destination).toString("base64") })}`,
      path,
      basename(path),
    );
  }
  private query(pkg: string, path: string): URLSearchParams {
    return new URLSearchParams({
      knownfolderid: "LocalAppData",
      packagefullname: pkg,
      path: path ? `/${path.replace(/^[/\\]+/, "")}` : "",
    });
  }
  async listFiles(pkg: string, path: string): Promise<{ Name: string }[]> {
    const response = await this.raw("GET", `/api/filesystem/apps/files?${this.query(pkg, path)}`);
    const data = (await response.json()) as { Items?: { Name: string }[] };
    return data.Items ?? [];
  }
  async makeFolder(pkg: string, parent: string, name: string): Promise<void> {
    const query = this.query(pkg, parent);
    query.set("newfoldername", name);
    try {
      await this.request("POST", `/api/filesystem/apps/folder?${query}`);
    } catch (cause) {
      // Only ignore a create failure after proving that exact directory exists.
      const data = (await (
        await this.request("GET", `/api/filesystem/apps/files?${this.query(pkg, parent)}`)
      ).json()) as { Items?: { Name: string; Type?: number }[] };
      if (!data.Items?.some((item) => item.Name === name)) throw cause;
    }
  }
  async pushFile(
    pkg: string,
    path: string,
    directory: string,
    remoteName = basename(path),
    onProgress?: (bytes: number, total: number) => void,
  ): Promise<void> {
    const query = this.query(pkg, directory);
    query.set("extract", "false");
    await this.sendFile(`/api/filesystem/apps/file?${query}`, path, remoteName, onProgress);
    const data = (await (
      await this.request("GET", `/api/filesystem/apps/files?${query}`)
    ).json()) as { Items?: { Name: string; FileSize?: number; SizeInBytes?: number }[] };
    const item = data.Items?.find((item) => item.Name === remoteName);
    if (!item || Number(item.FileSize ?? item.SizeInBytes) !== Bun.file(path).size)
      throw new PackError("portal");
  }
}
