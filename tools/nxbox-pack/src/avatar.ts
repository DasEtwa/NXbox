// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { extname } from "node:path";
import { PackError, type Language, strings } from "./i18n";
import { encodeAvatarJpeg } from "./jpeg";

/** The user's browser supplies its image decoders; Bun encodes JPEG in pure TypeScript.
 * The loopback server is ephemeral, token-scoped, and never accepts paths from requests. */
export async function convertAvatar(path: string, language: Language): Promise<Uint8Array> {
  if (!/\.(png|jpe?g|webp|gif|bmp)$/i.test(extname(path))) throw new PackError("avatarInvalid");
  const file = Bun.file(path);
  if (!(await file.exists()) || file.size === 0 || file.size > 20 * 1024 * 1024)
    throw new PackError("avatarInvalid");
  const bytes = await file.bytes();
  const text = strings[language];
  const token = crypto.randomUUID();
  let finish!: (rgba: Uint8Array) => void;
  let fail!: (error: Error) => void;
  const result = new Promise<Uint8Array>((resolve, reject) => {
    finish = resolve;
    fail = reject;
  });
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    maxRequestBodySize: 256 * 256 * 4,
    async fetch(request) {
      const url = new URL(request.url);
      const base = `http://127.0.0.1:${server.port}`;
      if (url.host !== `127.0.0.1:${server.port}` || !url.pathname.startsWith(`/${token}/`))
        return new Response(null, { status: 404 });
      if (request.method === "GET" && url.pathname === `/${token}/image`)
        return new Response(bytes, {
          headers: { "Content-Type": "application/octet-stream", "Cache-Control": "no-store" },
        });
      if (request.method === "POST" && request.headers.get("origin") === base) {
        if (url.pathname === `/${token}/rgba`) {
          const rgba = new Uint8Array(await request.arrayBuffer());
          if (rgba.length !== 256 * 256 * 4) return new Response(null, { status: 400 });
          finish(rgba);
          return new Response("ok");
        }
        if (url.pathname === `/${token}/error`) fail(new PackError("avatarInvalid"));
        return new Response("ok");
      }
      if (request.method !== "GET" || url.pathname !== `/${token}/`)
        return new Response(null, { status: 404 });
      // Text is inserted as textContent through JSON serialization, never interpreted as markup.
      return new Response(
        `<!doctype html><meta charset="utf-8"><title>NXbox</title><p id="status"></p><script>
      const status=document.getElementById('status');status.textContent=${JSON.stringify(text.avatarConverting)};
      (async()=>{try {
        const blob=await (await fetch('./image')).blob();
        const image=await createImageBitmap(blob);
        if(image.width*image.height>40000000) throw Error('size');
        const canvas=document.createElement('canvas');canvas.width=canvas.height=256;
        const ctx=canvas.getContext('2d'); const side=Math.min(image.width,image.height);
        ctx.drawImage(image,(image.width-side)/2,(image.height-side)/2,side,side,0,0,256,256);
        image.close(); const rgba=ctx.getImageData(0,0,256,256).data;
        const response=await fetch('./rgba',{method:'POST',body:rgba});if(!response.ok)throw Error('upload');
        status.textContent=${JSON.stringify(text.avatarConverted)};
      }catch(e){status.textContent=${JSON.stringify(text.avatarInvalid)};await fetch('./error',{method:'POST'});}})();
      </script>`,
        {
          headers: {
            "Content-Type": "text/html; charset=utf-8",
            "Cache-Control": "no-store",
            "Content-Security-Policy":
              "default-src 'none'; script-src 'unsafe-inline'; connect-src 'self'; img-src blob:; base-uri 'none'; frame-ancestors 'none'",
          },
        },
      );
    },
  });
  const timeout = setTimeout(() => fail(new PackError("avatarInvalid")), 120_000);
  const url = `http://127.0.0.1:${server.port}/${token}/`;
  console.log(`${text.avatarBrowser}\n${url}`);
  const opener =
    process.platform === "darwin"
      ? ["open", url]
      : process.platform === "win32"
        ? ["rundll32.exe", "url.dll,FileProtocolHandler", url]
        : ["xdg-open", url];
  try {
    // A manual local URL is printed if the desktop has no registered browser opener.
    try {
      Bun.spawn(opener, { stdout: "ignore", stderr: "ignore" });
    } catch {
      /* Use the printed URL. */
    }
    return encodeAvatarJpeg(await result);
  } finally {
    clearTimeout(timeout);
    server.stop(false);
  }
}
