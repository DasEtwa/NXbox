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
export function generateManifest(titleId: string, name: string, version = "0.1.0.0"): string {
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
  if (XMLValidator.validate(result) !== true) throw new Error("Invalid manifest XML");
  return result;
}
