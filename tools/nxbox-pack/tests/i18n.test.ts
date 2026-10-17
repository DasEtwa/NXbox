// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, test } from "bun:test";
import { readdir } from "node:fs/promises";
import {
  detectLanguage,
  matchLanguage,
  matchPreferences,
  registry,
  strings,
  withEnglishFallback,
} from "../src/i18n/registry";
import en from "../src/i18n/en";

function placeholders(text: string): string[] {
  return (
    text.match(
      /%%|%(?:\d+\$)?[-+#0 ']*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]|\{[A-Za-z_][A-Za-z_0-9]*\}/g,
    ) || []
  )
    .filter((token) => token !== "%%")
    .sort();
}

describe("community translations", () => {
  test("every language file contains only known keys and matching placeholders", async () => {
    for (const file of await readdir(new URL("../src/i18n", import.meta.url))) {
      if (!file.endsWith(".ts") || file === "registry.ts") continue;
      const table = (await import(`../src/i18n/${file}`)).default;
      expect(Object.values(registry).some((entry) => entry.table === table)).toBe(true);
      for (const [key, value] of Object.entries(table)) {
        expect(Object.hasOwn(en, key)).toBe(true);
        expect(typeof value).toBe("string");
        expect(placeholders(value as string)).toEqual(placeholders(en[key as keyof typeof en]));
      }
    }
    expect(Object.values(en).every(Boolean)).toBe(true);
    for (const table of Object.values(strings))
      expect(Object.values(table).every(Boolean)).toBe(true);
  });

  test("exact tags, primary subtags and preference order", () => {
    const languages = {
      en: { tag: "en" },
      br: { tag: "pt-BR" },
      pt: { tag: "pt-PT" },
      es: { tag: "es" },
      zh: { tag: "zh-Hans" },
    };
    for (const [tag, expected] of [
      ["pt-PT", "pt"],
      ["pt-BR", "br"],
      ["es-ES", "es"],
      ["es-MX", "es"],
      ["zh-Hans-CN", "zh"],
      ["unknown", "en"],
    ] as const)
      expect(matchPreferences([tag], languages)).toBe(expected);
    expect(matchPreferences(["unknown", "es-MX", "pt-BR"], languages)).toBe("es");
    expect(matchLanguage("ptx", languages)).toBeUndefined();
    expect(matchLanguage("", languages)).toBeUndefined();
    expect(detectLanguage({}, ["unknown", "pt-PT"])).toBe("pt");
    expect(detectLanguage({ NXBOX_PACK_LANG: "PT-br", LANG: "en_US" })).toBe("pt");
    expect(detectLanguage({ NXBOX_PACK_LANG: "unknown", LANG: "pt_PT.UTF-8" })).toBe("pt");
  });

  test("missing and empty entries fall back per key", () => {
    const text = withEnglishFallback({ guide: "Translated", legal: "" });
    expect(text.guide).toBe("Translated");
    expect(text.legal).toBe(en.legal);
    expect(text.keys).toBe(en.keys);
    expect(placeholders("%s {name} %s %%")).toEqual(["%s", "%s", "{name}"]);
    expect(placeholders("%s {name}")).not.toEqual(placeholders("%d {other}"));
  });
});
