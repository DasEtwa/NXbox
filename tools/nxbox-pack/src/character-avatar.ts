import { unzlibSync } from "fflate";
import catalog from "./avatar-catalog.json";
import { PackError } from "./i18n";
import { encodeAvatarJpeg } from "./jpeg";

export const avatarCharacters = catalog.characters.map(({ id }) => id);
export const avatarColors = catalog.colors;

/** Alpha-over for premultiplied RGBA, matching the console's WIC pixels. */
export function compositeAvatar(pixels: Uint8Array, background: readonly number[]): Uint8Array {
  if (pixels.length % 4 || background.length !== 3) throw new PackError("avatarInvalid");
  const result = pixels.slice();
  for (let i = 0; i < result.length; i += 4) {
    const alpha = pixels[i + 3]!;
    for (let channel = 0; channel < 3; ++channel)
      result[i + channel] = Math.min(
        255,
        pixels[i + channel]! + Math.floor((background[channel]! * (255 - alpha) + 127) / 255),
      );
    result[i + 3] = 255;
  }
  return result;
}

export function characterAvatar(character: string, color: string): Uint8Array {
  const entry = catalog.characters.find(({ id }) => id === character);
  const background = catalog.colors.find(({ name }) => name === color);
  if (!entry || !background) throw new PackError("profileUsage");
  const pixels = unzlibSync(Buffer.from(entry.rgba, "base64"));
  return encodeAvatarJpeg(compositeAvatar(pixels, background.rgb));
}
