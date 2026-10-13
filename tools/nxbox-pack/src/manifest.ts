import template from "../../../dist/nxbox/AppxManifest.xml" with { type: "text" };
import { XMLValidator } from "fast-xml-parser";
import { validateName, validateTitleId } from "./files";
export function packageIdentity(titleId: string): string {
  return `NSPX.NXbox.${validateTitleId(titleId)}`;
}
export function escapeXml(text: string): string {
  return text.replace(
    /[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&apos;" })[c]!,
  );
}
// Re-registering the same identity at the same version from a new folder fails on the console,
// so every pack gets a newer version: an in-place upgrade that keeps LocalState (saves).
export function packageVersion(now = new Date()): string {
  const days = Math.floor((now.getTime() - Date.UTC(2026, 0, 1)) / 86_400_000);
  const twoSecondSteps = Math.floor((now.getTime() % 86_400_000) / 2_000);
  return `0.2.${days}.${twoSecondSteps}`;
}
export function generateManifest(
  titleId: string,
  name: string,
  version = packageVersion(),
  /** Package-relative image for every tile size; the dashboard scales it. */
  logo?: string,
): string {
  if (!/^\d+\.\d+\.\d+\.\d+$/.test(version) || version.split(".").some((n) => Number(n) > 65535))
    throw new Error("Invalid package version");
  const displayName = escapeXml(validateName(name));
  const result = template
    .replace('Name="NSPX.NXbox"', `Name="${packageIdentity(titleId)}"`)
    .replace('Version="0.1.0.0"', `Version="${version}"`)
    .replace(/<Extensions>[\s\S]*?<\/Extensions>/, "")
    .replace(/>NXbox</g, `>${displayName}<`)
    .replace(
      /DisplayName="NXbox"|Description="NXbox"/g,
      (attr) => `${attr.split("=")[0]}="${displayName}"`,
    );
  const tiled = logo
    ? result.replace(/(Logo>|Logo=")Assets\\[^<"]+\.png/g, `$1${escapeXml(logo)}`)
    : result;
  if (XMLValidator.validate(tiled) !== true) throw new Error("Invalid manifest XML");
  return tiled;
}
