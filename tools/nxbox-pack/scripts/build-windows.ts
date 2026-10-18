// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// Bun 1.3.14 fails to extract its cross-target runtime ZIP on Windows runners.
// Native Windows builds can embed the already-installed executable instead.
const child = Bun.spawn(
  [
    process.execPath,
    "build",
    "src/cli.ts",
    "--compile",
    ...(process.platform === "win32" ? [] : ["--target=bun-windows-x64-baseline"]),
    "--outfile",
    "dist/nxbhelper-windows-x64.exe",
  ],
  { stdin: "inherit", stdout: "inherit", stderr: "inherit" },
);
process.exitCode = await child.exited;

export {};
