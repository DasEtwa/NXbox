// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

export { detectLanguage, strings } from "./i18n/registry";
export type { Language, Message } from "./i18n/registry";
import { strings, type Language, type Message } from "./i18n/registry";
import { formatSize } from "./checklist";

export class PackError extends Error {
  readonly missingBytes?: number;
  localized(language: Language): string {
    if (this.code === "consoleFull" && this.missingBytes !== undefined)
      return strings[language].consoleFullSpace.replace("{size}", formatSize(this.missingBytes));
    return strings[language][this.code];
  }
  constructor(
    readonly code: Message,
    options?: ErrorOptions & { missingBytes?: number },
  ) {
    super(code, options);
    this.missingBytes = options?.missingBytes;
  }
}
