#!/usr/bin/env bun
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { checkDrive, fixDrive, type DriveReport } from "./drive";
import { runJob, type Job } from "./api";
import { detectLanguage, PackError, strings } from "./i18n";
import {
  draggedPath,
  filenameName,
  parseTitleId,
  validateFile,
  validateName,
  validateTitleId,
} from "./files";
import { deployGame } from "./deploy";
import {
  downloadRuntime,
  latestRuntime,
  stageRuntime,
  validateSource,
  type RuntimeSource,
} from "./runtime";
import { characterAvatar } from "./character-avatar";
import { convertAvatar } from "./avatar";
import { parseProfileArgs, setMulti, uploadAvatar } from "./profile";
import { DevicePortal, portalUrl } from "./portal";
import { discoverXbox } from "./discovery";
import { loadCredentials, saveCredentials } from "./credentials";
import { prepareContent } from "./content";
import { checkAddon, renderChecklist, type Item } from "./checklist";
import { Prompt } from "./prompt";
import { prepareFirmware } from "./firmware";
import {
  prepareSaves,
  prepareMods,
  prepareTitleKeys,
  type Transfer,
  type OptionalContent,
} from "./optional";

export function parseArgs(args: string[]): {
  help: boolean;
  insecure: boolean;
  source?: RuntimeSource;
} {
  let url: string | undefined, sha256: string | undefined;
  let insecure = false,
    help = false;
  for (let i = 0; i < args.length; i++) {
    switch (args[i]) {
      case "--help":
      case "-h":
        help = true;
        break;
      case "--insecure-portal":
        insecure = true;
        break;
      case "--runtime-url":
        url = args[++i];
        if (!url) throw new PackError("args");
        break;
      case "--runtime-sha256":
        sha256 = args[++i];
        if (!sha256) throw new PackError("args");
        break;
      default:
        throw new PackError("args");
    }
  }
  if (Boolean(url) !== Boolean(sha256)) throw new PackError("args");
  return { help, insecure, source: url && sha256 ? validateSource({ url, sha256 }) : undefined };
}
async function systemLanguage() {
  let locale = Intl.DateTimeFormat().resolvedOptions().locale;
  if (
    process.platform === "win32" &&
    !process.env.LANG &&
    !process.env.LC_ALL &&
    !(process.env.NXHELPER_LANG ?? process.env.NXBOX_PACK_LANG)
  ) {
    try {
      const localeProcess = Bun.spawn(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", "(Get-Culture).Name"],
        { stdout: "pipe", stderr: "ignore" },
      );
      const value = (await new Response(localeProcess.stdout).text()).trim();
      if ((await localeProcess.exited) === 0 && value) locale = value;
    } catch {
      /* Intl remains the fallback. */
    }
  }
  return detectLanguage(process.env, locale);
}
export async function main(): Promise<void> {
  const language = await systemLanguage();
  const text = strings[language];
  let prompt: Prompt | undefined, temporary: string | undefined;
  try {
    if (process.argv[2] === "--api") return await (await import("./api")).serveApi();
    if (process.argv[2] === "profile") {
      const { command, insecure } = parseProfileArgs(process.argv.slice(3));
      temporary = await mkdtemp(join(tmpdir(), "nxbox-profile-"));
      // Decode before contacting the console; invalid images never change console state.
      const jpeg =
        command.kind === "avatar"
          ? await convertAvatar(draggedPath(command.image), language)
          : command.kind === "character"
            ? characterAvatar(command.character, command.color)
            : undefined;
      prompt = new Prompt();
      const discovered = await discoverXbox();
      const base = portalUrl(
        await prompt.ask(text.host, discovered.length === 1 ? discovered[0] : ""),
      );
      const credentials = (await loadCredentials(base)) ?? {
        user: await prompt.ask(text.user),
        password: await prompt.password(text.password),
      };
      const portal = new DevicePortal(base, credentials, insecure);
      const packages = (await portal.packages()).filter((pkg) =>
        /^NSPX\.NXbox(?:[._]|$)/i.test(pkg.PackageFullName),
      );
      if (!packages.length) throw new PackError("profileMissing");
      for (const pkg of packages) console.log(`${pkg.Name}: ${pkg.PackageFullName}`);
      const selected = await prompt.ask(
        text.profilePackage,
        packages.length === 1 ? packages[0]!.PackageFullName : "",
      );
      if (!packages.some((pkg) => pkg.PackageFullName === selected))
        throw new PackError("profileMissing");
      await prompt.ask(text.profileClose);
      if (command.kind === "multi") await setMulti(portal, selected, command.enabled, temporary);
      else await uploadAvatar(portal, selected, command.name, jpeg!, temporary);
      console.log(text.profileDone);
      return;
    }
    const printReport = (report: DriveReport) => {
      for (const item of report.items) {
        console.log(
          `${{ ok: "✓", warn: "!", fail: "✗" }[item.status]} ${item.title}${item.detail ? ` — ${item.detail}` : ""}`,
        );
        if (item.fix) console.log(`  ${item.fix}`);
      }
      console.log(report.ready ? text.driveReady : text.driveNotReady);
      process.exitCode = report.ready ? 0 : 1;
    };
    const args = process.argv.slice(2);
    if (args.includes("--check-drive")) {
      const index = args.indexOf("--check-drive");
      if (!args[index + 1] || args.some((a, i) => i !== index && i !== index + 1 && a !== "--fix"))
        throw new PackError("args");
      const root = draggedPath(args[index + 1]!);
      const report = await checkDrive(root, language);
      printReport(args.includes("--fix") ? await fixDrive(root, report, language) : report);
      return;
    }
    const transfer = async (
      mode: "drive" | "xbox",
      argv: string[],
      interactive: boolean,
      destination?: string,
    ) => {
      const values: Record<string, string> = {};
      let insecure = false;
      const games: string[] = [];
      for (let i = 0; i < argv.length; i++) {
        const arg = argv[i]!;
        if (arg === "--insecure-portal") {
          insecure = true;
          continue;
        }
        if (["--keys", "--title-keys", "--firmware", "--host"].includes(arg)) {
          const value = argv[++i];
          if (!value || value.startsWith("--")) throw new PackError("args");
          values[arg] = value;
        } else if (arg.startsWith("--")) throw new PackError("args");
        else games.push(draggedPath(arg));
      }
      prompt ??= new Prompt();
      const root =
        mode === "drive"
          ? (destination ??
            (interactive ? draggedPath(await prompt.ask(text.drivePath)) : undefined))
          : undefined;
      if (interactive) {
        for (;;) {
          const path = await prompt.ask(text.gamePaths);
          if (!path) break;
          games.push(draggedPath(path));
        }
      }
      const keys = values["--keys"] ?? draggedPath(await prompt.ask(text.keys));
      if (!keys || !games.length || (mode === "drive" && !root)) throw new PackError("args");
      let titleKeys = values["--title-keys"],
        firmware = values["--firmware"];
      if (interactive) {
        titleKeys = (await prompt.ask(text.titleKeys)) || undefined;
        firmware = (await prompt.ask(text.firmware)) || undefined;
      }
      let target: Job["target"];
      if (mode === "drive") target = { kind: "export", out: root! };
      else {
        const discovered = values["--host"] ? [] : await discoverXbox();
        const base = portalUrl(
          values["--host"] ??
            (await prompt.ask(text.host, discovered.length === 1 ? discovered[0] : "")),
        );
        const credentials = (await loadCredentials(base)) ?? {
          user: await prompt.ask(text.user),
          password: await prompt.password(text.password),
        };
        target = {
          kind: "install",
          host: base,
          ...credentials,
          insecure,
          remember: /^(y|yes|s|sim)$/i.test(await prompt.ask(text.remember)),
        };
      }
      await runJob(
        {
          mode,
          games,
          keys: draggedPath(keys),
          titleKeys: titleKeys ? draggedPath(titleKeys) : undefined,
          firmware: firmware ? draggedPath(firmware) : undefined,
          language,
          target,
        },
        (event) => {
          if (event.report) printReport(event.report as DriveReport);
          else if (event.warning) console.log(event.warning);
          else if (event.file) console.log(`${event.file}: ${event.sent}/${event.total}`);
          else if (event.done) console.log(text.transferDone);
        },
      );
    };
    if (args[0] === "--to-drive") {
      const root = args[1];
      if (!root) throw new PackError("args");
      await transfer("drive", args.slice(2), false, draggedPath(root));
      return;
    }
    if (args[0] === "--to-xbox") {
      await transfer("xbox", args.slice(1), false);
      return;
    }
    const options = parseArgs(args);
    if (options.help) {
      console.log(`${text.help}\n${text.driveUsage}\n${text.profileUsage}`);
      return;
    }
    console.log(`NxHelper\n${text.legal}`);
    prompt = new Prompt();
    const way = await prompt.ask(text.wayChoice, "1");
    if (way === "1" || way === "2") {
      await transfer(way === "1" ? "drive" : "xbox", [], true);
      return;
    }
    if (way !== "3") throw new PackError("args");
    console.log(text.guide);
    const game = draggedPath(await prompt.ask("→"));
    await validateFile(game);
    const titleId = validateTitleId(parseTitleId(game) ?? (await prompt.ask(text.title)));
    const gameInfo = { titleId, size: Bun.file(game).size };
    const keys = draggedPath(await prompt.ask(text.keys));
    await validateFile(keys, true);
    temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-"));
    const transfers: Transfer[] = [];
    const checklist: OptionalContent = {};
    const extras: string[] = [];
    const added: Item[] = [];
    const show = () => console.log(renderChecklist(text, gameInfo, true, added, checklist));
    show();
    for (;;) {
      const answer = await prompt.ask(text.extra);
      if (!answer) break;
      const path = draggedPath(answer);
      await validateFile(path);
      const result = checkAddon(titleId, path, Bun.file(path).size, added);
      if (typeof result === "string") console.log(text[result]);
      else {
        added.push(result);
        extras.push(path);
      }
      show();
    }
    for (const kind of ["firmware", "saves", "mods", "titleKeys"] as const) {
      for (;;) {
        const answer = await prompt.ask(text[kind]);
        if (!answer) break;
        const path = draggedPath(answer);
        const staging = join(temporary, `${kind}-${crypto.randomUUID()}`);
        try {
          if (kind === "firmware") {
            const prepared = await prepareFirmware(path, keys, staging);
            transfers.push(
              ...prepared.files.map((f) => ({
                path: f.path,
                destination: `LocalState/firmware/${f.name}`,
              })),
            );
            checklist.firmware = `${prepared.version} — ${prepared.files.length} ${text.fileCount}`;
          } else if (kind === "titleKeys") {
            transfers.push(await prepareTitleKeys(path));
            checklist.titleKeys = "title.keys";
          } else {
            const prepared = await (kind === "saves" ? prepareSaves : prepareMods)(
              path,
              titleId,
              staging,
            );
            if (
              prepared.files.some((f) =>
                transfers.some((t) => t.destination.toLowerCase() === f.destination.toLowerCase()),
              )
            )
              throw new PackError("rejectDuplicate");
            transfers.push(...prepared.files);
            checklist[kind] =
              `${kind === "mods" ? transfers.filter((f) => f.destination.startsWith("LocalState/eden/load/")).length : prepared.count} ${text.fileCount}`;
          }
          show();
          if (kind !== "mods") break;
        } catch (error) {
          console.log(text[error instanceof PackError ? error.code : "inputInvalid"]);
          await rm(staging, { recursive: true, force: true });
          show();
        }
      }
    }
    console.log(text.fallback);
    const name = validateName(await prompt.ask(text.name, filenameName(game)));
    // Resolve the runtime before collecting credentials or changing console state.
    const source = options.source ?? (await latestRuntime());
    console.log(text.runtime);
    const runtime = await downloadRuntime(source);
    const directory = join(temporary, "runtime");
    const files = await stageRuntime(runtime, directory, titleId, name);
    const content: string[] = [];
    for (const [index, path] of [game, ...extras].entries())
      content.push(await prepareContent(path, join(temporary, `content-${index}`), keys));
    const discovered = await discoverXbox();
    if (discovered.length) console.log(`${text.found}: ${discovered.join(", ")}`);
    const base = portalUrl(
      await prompt.ask(text.host, discovered.length === 1 ? discovered[0] : ""),
    );
    let credentials = await loadCredentials(base);
    const yes = (answer: string) => /^(y|yes|s|sim)$/i.test(answer);
    if (credentials && !yes(await prompt.ask(text.saved, language === "pt" ? "s" : "y")))
      credentials = undefined;
    if (!credentials) {
      credentials = {
        user: await prompt.ask(text.user),
        password: await prompt.password(text.password),
      };
      if (!credentials.user || credentials.user.includes(":") || !credentials.password)
        throw new PackError("portal");
    }
    const portal = new DevicePortal(base, credentials, options.insecure);
    await portal.packages(); // Authenticate before offering to persist credentials.
    if (yes(await prompt.ask(text.remember))) {
      if (!(await saveCredentials(base, credentials))) console.log(text.secureUnavailable);
    }
    console.log(text.install);
    let lastProgress = 0;
    const identity = await deployGame({
      portal,
      titleId,
      runtimeDirectory: directory,
      runtimeFiles: files,
      content,
      keys,
      temporary,
      transfers,
      onUploading: () => console.log(text.upload),
      onProgress: (remoteName, sent, total) => {
        if (Date.now() - lastProgress > 5000 || sent === total) {
          console.log(`${text.progress} ${remoteName}: ${((sent / total) * 100).toFixed(1)}%`);
          lastProgress = Date.now();
        }
      },
    });
    console.log(`${name} (${identity})\n${text.done}`);
  } catch (error) {
    const code = error instanceof PackError ? error.code : "failed";
    console.error(
      `${text.failed}: ${error instanceof PackError ? error.localized(language) : text[code]}`,
    );
    process.exitCode = 1;
  } finally {
    prompt?.close();
    if (temporary) await rm(temporary, { recursive: true, force: true });
  }
}
if (import.meta.main) await main();
