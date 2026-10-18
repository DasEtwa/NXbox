// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import en from "./en";
import pt from "./pt_br";

export type Message = keyof typeof en;
export type Translation = Partial<Record<Message, string>>;
export type Messages = Record<Message, string>;

// Internal identifiers stay stable; tags are the public BCP-47 language names.
export const registry = {
  en: { tag: "en", table: en },
  pt: { tag: "pt-BR", table: pt },
} satisfies Record<string, { tag: string; table: Translation }>;
export type Language = keyof typeof registry;

export function normalizeTag(tag: string): string {
  return tag.split(/[.@]/, 1)[0]!.replaceAll("_", "-").toLowerCase();
}

export function matchLanguage<Id extends string>(
  preference: string,
  languages: Record<Id, { tag: string }>,
): Id | undefined {
  const tag = normalizeTag(preference);
  if (!tag) return undefined;
  const entries = Object.entries(languages) as [Id, { tag: string }][];
  const primary = tag.split("-", 1)[0]!;
  return (entries.find(([, entry]) => normalizeTag(entry.tag) === tag) ??
    entries.find(([, entry]) => normalizeTag(entry.tag) === primary) ??
    entries.find(([, entry]) => normalizeTag(entry.tag).split("-", 1)[0] === primary))?.[0];
}

export function matchPreferences<Id extends string>(
  preferences: readonly string[],
  languages: Record<Id, { tag: string }> & Record<"en", { tag: string }>,
): Id | "en" {
  for (const preference of preferences) {
    const match = matchLanguage(preference, languages);
    if (match !== undefined) return match;
  }
  return "en";
}

export function withEnglishFallback(table: Translation): Messages {
  return Object.fromEntries(
    Object.entries(en).map(([key, value]) => [key, table[key as Message] || value]),
  ) as Messages;
}

export const strings = Object.fromEntries(
  Object.entries(registry).map(([id, entry]) => [id, withEnglishFallback(entry.table)]),
) as Record<Language, Messages>;

export function detectLanguage(
  env: Record<string, string | undefined> = process.env,
  locale: string | readonly string[] = Intl.DateTimeFormat().resolvedOptions().locale,
): Language {
  const forced = matchLanguage((env.NXHELPER_LANG ?? env.NXBOX_PACK_LANG) || "", registry);
  if (forced !== undefined) return forced;
  const environment = env.LC_ALL || env.LC_MESSAGES || env.LANG;
  return matchPreferences(
    environment ? [environment] : typeof locale === "string" ? [locale] : locale,
    registry,
  );
}
