// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, test } from "bun:test";
import {
  checkAddon,
  classifyAddon,
  fileTitleId,
  formatSize,
  renderChecklist,
} from "../src/checklist";
import { strings } from "../src/i18n";

const base = "01007EF00011E000";
describe("add-on checklist", () => {
  test("classifies update and DLC, rejects others", () => {
    expect(classifyAddon(base, "01007EF00011E800")).toBe("update");
    expect(classifyAddon(base, "01007EF00011F001")).toBe("dlc");
    expect(classifyAddon(base, "01007EF00011E001")).toBeUndefined();
    expect(classifyAddon(base, "0100AAAA0011F001")).toBeUndefined();
  });
  test("reasons for rejected files", () => {
    const first = checkAddon(base, "/x/U [01007EF00011E800][v65536].nsp", 10, []);
    expect(first).toMatchObject({ kind: "update", titleId: "01007EF00011E800" });
    expect(checkAddon(base, "/x/noid.nsp", 1, [])).toBe("rejectNoId");
    expect(checkAddon(base, "/x/O [0100AAAA0011E800].nsp", 1, [])).toBe("rejectMismatch");
    expect(checkAddon(base, "/x/U [01007EF00011E800].nsp", 1, [first as never])).toBe(
      "rejectDuplicate",
    );
    expect(fileTitleId("a [01007EF00011E800] [01007EF00011E000].nsp")).toBeUndefined();
  });
  test("renders running list with sizes", () => {
    const item = checkAddon(base, "/x/D [01007EF00011F001].nsp", 2048, []);
    const out = renderChecklist(strings.en, { titleId: base, size: 3 * 1024 ** 3 }, true, [
      item as never,
    ]);
    expect(out).toContain("[x] Game 01007EF00011E000 (3.0 GiB)");
    expect(out).toContain("[ ] Update -");
    expect(out).toContain("[x] DLC 01007EF00011F001 (2.0 KiB)");
    expect(formatSize(5)).toBe("5 B");
  });
});
