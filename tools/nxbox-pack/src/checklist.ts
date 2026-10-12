import { basename } from "node:path";

export type ItemKind = "update" | "dlc";
export type Rejection = "rejectNoId" | "rejectMismatch" | "rejectDuplicate";
export type Item = { kind: ItemKind; path: string; titleId: string; size: number };

// Any single `[0100XXXXXXXXXXXX]` token; unlike a base ID it may end in 800 (update) or anything (DLC).
export function fileTitleId(path: string): string | undefined {
  const matches = [...basename(path).matchAll(/\[(0100[0-9a-f]{12})\]/gi)];
  return matches.length === 1 ? matches[0]![1]!.toUpperCase() : undefined;
}
// Update = base | 0x800. DLC = the 0x1000 block right after the base, any low 12 bits.
export function classifyAddon(base: string, id: string): ItemKind | undefined {
  const b = BigInt(`0x${base}`),
    v = BigInt(`0x${id}`);
  if (v === (b | 0x800n)) return "update";
  if (v >> 12n === (b >> 12n) + 1n) return "dlc";
  return undefined;
}
export function checkAddon(
  base: string,
  path: string,
  size: number,
  added: Item[],
): Item | Rejection {
  const titleId = fileTitleId(path);
  if (!titleId) return "rejectNoId";
  const kind = classifyAddon(base, titleId);
  if (!kind) return "rejectMismatch";
  if (added.some((i) => i.titleId === titleId || i.path === path)) return "rejectDuplicate";
  return { kind, path, titleId, size };
}
export function formatSize(bytes: number): string {
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let value = bytes,
    unit = 0;
  while (value >= 1024 && unit < units.length - 1) ((value /= 1024), unit++);
  return `${unit ? value.toFixed(1) : value} ${units[unit]}`;
}
type Labels = {
  itemGame: string;
  itemKeys: string;
  itemUpdate: string;
  itemDlc: string;
  none: string;
  itemFirmware?: string;
  itemSaves?: string;
  itemMods?: string;
  itemTitleKeys?: string;
};
export function renderChecklist(
  labels: Labels,
  game: { titleId: string; size: number },
  keys: boolean,
  items: Item[],
  optional: { firmware?: string; saves?: string; mods?: string; titleKeys?: string } = {},
): string {
  const line = (done: boolean, label: string, detail: string) =>
    `  [${done ? "x" : " "}] ${label}${detail ? ` ${detail}` : ""}`;
  const rows = [
    line(true, labels.itemGame, `${game.titleId} (${formatSize(game.size)})`),
    line(keys, labels.itemKeys, ""),
  ];
  for (const kind of ["update", "dlc"] as const) {
    const found = items.filter((i) => i.kind === kind);
    const label = kind === "update" ? labels.itemUpdate : labels.itemDlc;
    if (!found.length) rows.push(line(false, label, labels.none));
    for (const i of found) rows.push(line(true, label, `${i.titleId} (${formatSize(i.size)})`));
  }
  for (const [field, label] of [
    ["firmware", labels.itemFirmware],
    ["saves", labels.itemSaves],
    ["mods", labels.itemMods],
    ["titleKeys", labels.itemTitleKeys],
  ] as const) {
    if (label) rows.push(line(Boolean(optional[field]), label, optional[field] ?? labels.none));
  }
  return rows.join("\n");
}
