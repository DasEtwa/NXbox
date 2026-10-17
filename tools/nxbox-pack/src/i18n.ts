// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

export { detectLanguage, strings } from "./i18n/registry";
export type { Language, Message } from "./i18n/registry";
import type { Message } from "./i18n/registry";

export class PackError extends Error {
  constructor(
    readonly code: Message,
    options?: ErrorOptions,
  ) {
    super(code, options);
  }
}
