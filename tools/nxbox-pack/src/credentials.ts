// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { createHash } from "node:crypto";
import { homedir } from "node:os";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import type { Credentials } from "./portal";

async function command(args: string[], input?: string): Promise<string | undefined> {
  try {
    const child = Bun.spawn(args, {
      stdin: input === undefined ? "ignore" : "pipe",
      stdout: "pipe",
      stderr: "ignore",
    });
    if (input !== undefined && child.stdin) {
      child.stdin.write(input);
      child.stdin.end();
    }
    const timer = setTimeout(() => child.kill(), 30_000);
    try {
      const output = await new Response(child.stdout).text();
      return (await child.exited) === 0 ? output.trim() : undefined;
    } finally {
      clearTimeout(timer);
    }
  } catch {
    return undefined;
  }
}
const service = "dev.nspx.nxbox-pack";
function credentialPath(host: string): string {
  return join(
    homedir(),
    ".local",
    "share",
    "nxbox-pack",
    "credentials",
    `${createHash("sha256").update(host).digest("hex")}.dpapi`,
  );
}
async function dpapi(value: string, encrypt: boolean): Promise<string | undefined> {
  const operation = encrypt
    ? "[Convert]::ToBase64String([Security.Cryptography.ProtectedData]::Protect([Text.Encoding]::UTF8.GetBytes($v),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser))"
    : "[Text.Encoding]::UTF8.GetString([Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($v),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser))";
  return command(
    [
      "powershell.exe",
      "-NoProfile",
      "-NonInteractive",
      "-Command",
      `$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.Security; $v=[Console]::In.ReadToEnd(); ${operation}`,
    ],
    value,
  );
}
async function windowsVault(host: string, credentials?: Credentials): Promise<string | undefined> {
  const operation = credentials
    ? "$c=New-Object Windows.Security.Credentials.PasswordCredential('dev.nspx.nxbox-pack',$d.host,$d.record); $vault.Add($c)"
    : "$c=$vault.Retrieve('dev.nspx.nxbox-pack',$d.host); $c.RetrievePassword(); $c.Password";
  return command(
    [
      "powershell.exe",
      "-NoProfile",
      "-NonInteractive",
      "-Command",
      `$ErrorActionPreference='Stop'; $null=[Windows.Security.Credentials.PasswordVault,Windows.Security.Credentials,ContentType=WindowsRuntime]; $vault=New-Object Windows.Security.Credentials.PasswordVault; $d=[Console]::In.ReadToEnd() | ConvertFrom-Json; ${operation}`,
    ],
    JSON.stringify({ host, record: credentials ? JSON.stringify(credentials) : undefined }),
  );
}
export async function loadCredentials(host: string): Promise<Credentials | undefined> {
  let text: string | undefined;
  if (process.platform === "darwin")
    text = await command(["security", "find-generic-password", "-s", service, "-a", host, "-w"]);
  else if (process.platform === "linux")
    text = await command(["secret-tool", "lookup", "service", service, "host", host]);
  else if (process.platform === "win32") {
    text = await windowsVault(host);
    if (!text)
      try {
        text = await dpapi(await readFile(credentialPath(host), "utf8"), false);
      } catch {
        return undefined;
      }
  }
  if (!text) return undefined;
  try {
    const result = JSON.parse(text);
    if (typeof result.user === "string" && typeof result.password === "string") return result;
  } catch {
    /* Invalid secure-store records are never logged. */
  }
  return undefined;
}
export async function saveCredentials(host: string, credentials: Credentials): Promise<boolean> {
  const text = JSON.stringify(credentials);
  if (process.platform === "darwin") {
    // security's interactive input keeps the password out of process argv.
    const quote = (value: string) => `"${value.replaceAll("\\", "\\\\").replaceAll('"', '\\"')}"`;
    if (
      (await command(
        ["security", "-i"],
        `add-generic-password -U -s ${quote(service)} -a ${quote(host)} -w ${quote(text)}\n`,
      )) === undefined
    )
      return false;
    const saved = await loadCredentials(host);
    return saved?.user === credentials.user && saved?.password === credentials.password;
  }
  if (process.platform === "linux")
    return (
      (await command(
        ["secret-tool", "store", "--label=NxHelper", "service", service, "host", host],
        text,
      )) !== undefined
    );
  if (process.platform === "win32") {
    if ((await windowsVault(host, credentials)) !== undefined) return true;
    const encrypted = await dpapi(text, true);
    if (!encrypted) return false;
    try {
      const path = credentialPath(host);
      await mkdir(join(path, ".."), { recursive: true, mode: 0o700 });
      await writeFile(path, encrypted, { mode: 0o600 });
      return true;
    } catch {
      return false;
    }
  }
  return false;
}
