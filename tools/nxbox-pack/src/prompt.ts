// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { createInterface } from "node:readline/promises";
import { PackError } from "./i18n";
export class Prompt {
  private reader = createInterface({ input: process.stdin, output: process.stdout });
  async ask(message: string, fallback = ""): Promise<string> {
    const abort = new AbortController();
    const cancel = () => abort.abort();
    this.reader.once("close", cancel);
    this.reader.once("SIGINT", cancel);
    try {
      const value = (
        await this.reader.question(`${message}${fallback ? ` [${fallback}]` : ""}: `, {
          signal: abort.signal,
        })
      ).trim();
      return value || fallback;
    } catch {
      throw new PackError("interrupted");
    } finally {
      this.reader.off("close", cancel);
      this.reader.off("SIGINT", cancel);
    }
  }
  async password(message: string): Promise<string> {
    if (!process.stdin.isTTY || !process.stdin.setRawMode) throw new PackError("secret");
    this.reader.close();
    process.stdout.write(`${message}: `);
    const wasRaw = process.stdin.isRaw;
    process.stdin.setRawMode(true);
    return new Promise((resolve, reject) => {
      let value = "";
      const finish = (cancel = false) => {
        process.stdin.off("data", receive);
        process.stdin.setRawMode(wasRaw);
        this.reader = createInterface({ input: process.stdin, output: process.stdout });
        process.stdout.write("\n");
        cancel ? reject(new PackError("interrupted")) : resolve(value);
      };
      const receive = (data: Buffer) => {
        for (const char of data.toString("utf8")) {
          if (char === "\x03" || char === "\x04") {
            finish(true);
            return;
          }
          if (char === "\r" || char === "\n") {
            finish();
            return;
          }
          if (char === "\x7f" || char === "\b") value = [...value].slice(0, -1).join("");
          else if (char >= " " && char !== "\x1b") value += char;
        }
      };
      process.stdin.on("data", receive);
      process.stdin.resume();
    });
  }
  close(): void {
    this.reader.close();
  }
}
