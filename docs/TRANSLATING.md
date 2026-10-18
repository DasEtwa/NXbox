# Translating NXbox

NXbox's console UI and NxHelper ship in English and Brazilian Portuguese.
Each language has its own file; you can contribute either surface or both.
Track translations in [issue #10](https://github.com/NspxMiguel/NXbox/issues/10).

## Console UI

1. Copy `src/eden_uwp/ui/lang/en.inc` to a new file in the same directory,
   for example `es.inc` or `pt_pt.inc`.
2. Translate the `L"..."` values. Keep every `Text::Key` unchanged. Keep the
   `L` prefix, quotes, commas, escapes such as `\n` and `\\`, placeholders
   such as `%s` and `{name}`, and controller button names. Long values may
   use adjacent `L"..."` literals. Keep text short enough for a TV screen.
3. In `src/eden_uwp/ui/translation_tables.h`, add a table next to the existing
   tables:

   ```cpp
   inline constexpr TranslationEntry kSpanish[] = {
   #include "eden_uwp/ui/lang/es.inc"
   };
   ```

4. Register its identifier, BCP-47 tag and table in
   `src/eden_uwp/ui/lang/registry.inc`:

   ```cpp
   NXBOX_LANGUAGE(Spanish, "es", kSpanish)
   ```

   This also creates the `Language` enum member. Use `pt-PT` for Portuguese
   from Portugal, or `zh-Hans` for Simplified Chinese. Keep English first.
5. Run the host tests from the repository root:

   ```sh
   cd tests/port
   python3 -m unittest discover -s . -p 'test_*.py'
   ```

   To check just translations while editing, run
   `python3 -m unittest test_i18n` from that directory.

## NxHelper

1. Copy `tools/nxbox-pack/src/i18n/en.ts` to a new file in that directory,
   for example `es.ts`. Translate the values, keeping keys, placeholders,
   escapes, file extensions and command flags unchanged.
2. Use the translation type to catch unknown keys:

   ```ts
   import type { Translation } from "./registry";

   export default {
     guide: "Your translated text",
     // Continue with the other keys copied from en.ts.
   } satisfies Translation;
   ```

3. Import your file in `tools/nxbox-pack/src/i18n/registry.ts`, and add one
   registry entry:

   ```ts
   import es from "./es";
   // Inside registry:
   es: { tag: "es", table: es },
   ```

   The identifier is internal; the tag is the public BCP-47 language name.
   Keep English first. Existing `pt` identifies the `pt-BR` translation.
4. Install dependencies, format your new file, and run the tests:

   ```sh
   cd tools/nxbox-pack
   bun install --frozen-lockfile
   bunx prettier --write src/i18n/es.ts
   bun test
   bun run check
   ```

## Selection and fallback

For each system preference, NXbox tries the full tag first, then a registered
primary subtag, then the first registered regional variant of that primary
subtag. It moves to the next preference only if there is no match. For example,
`pt-PT` uses `pt-BR` today, but selects `pt-PT` once that tag is registered;
`es-MX` selects a registered `es`, and `zh-Hans-CN` can select `zh-Hans`.
Unknown languages fall back to English. Registry order chooses the regional
variant when there is no exact or primary tag match.

The console uses the Windows language preference list. NxHelper uses
`LC_ALL`, `LC_MESSAGES`, then `LANG` (the first nonempty value), or the Intl
locale when those variables are absent. Locale spellings such as `pt_BR.UTF-8`
are normalized. Tests can also pass a preference list to the NxHelper resolver.

For testing, set `NXBOX_LANG=es` on the console (process environment or
`LocalState/nxbox_env.txt`), or `NXBOX_PACK_LANG=es` on the computer. Both
accept any registered tag, case-insensitively, with the same matching rules.
An unknown override is ignored and system detection continues.

Incomplete translations are welcome: omit an untranslated key and English
will appear for that key. Empty values also fall back at runtime; omit keys
instead of leaving blank values. Tests check all language files for unknown
or duplicate console keys and matching placeholder names, types and counts.
English must contain nonempty text for every key.

## Submit

Open a pull request titled `Add <language> translation`, with one language per
pull request. State its BCP-47 tag and which surfaces you translated. A maintainer
will check the console layout. No console build is required to run the host tests.
