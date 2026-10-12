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
