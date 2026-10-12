import { createCipheriv, createDecipheriv, getCiphers } from "node:crypto";

const nativeXts = getCiphers().includes("aes-128-xts");
// Bun 1.3.14's BoringSSL omits XTS. Use its native AES-ECB primitives for XEX
// with the IEEE 1619 little-endian GF(2^128) tweak multiplication instead.
// NAND/header sectors are complete AES blocks, so ciphertext stealing is unnecessary.
export function xtsTransform(data: Buffer, key: Buffer, iv: Buffer, encrypt = false): Buffer {
  if (key.length !== 32 || iv.length !== 16 || data.length < 16 || data.length % 16)
    throw new Error("Invalid XTS sector");
  if (nativeXts) {
    const c = (encrypt ? createCipheriv : createDecipheriv)("aes-128-xts", key, iv);
    c.setAutoPadding(false);
    return Buffer.concat([c.update(data), c.final()]);
  }
  const e = createCipheriv("aes-128-ecb", key.subarray(16), null);
  e.setAutoPadding(false);
  const tweak = Buffer.concat([e.update(iv), e.final()]),
    tweaks = Buffer.alloc(data.length),
    input = Buffer.alloc(data.length);
  for (let offset = 0; offset < data.length; offset += 16) {
    tweak.copy(tweaks, offset);
    for (let i = 0; i < 16; i++) input[offset + i] = data[offset + i]! ^ tweak[i]!;
    let carry = 0;
    for (let i = 0; i < 16; i++) {
      const b = tweak[i]!;
      tweak[i] = ((b << 1) | carry) & 255;
      carry = b >>> 7;
    }
    if (carry) tweak[0] = tweak[0]! ^ 0x87;
  }
  const c = (encrypt ? createCipheriv : createDecipheriv)("aes-128-ecb", key.subarray(0, 16), null);
  c.setAutoPadding(false);
  const output = Buffer.concat([c.update(input), c.final()]);
  for (let i = 0; i < output.length; i++) output[i] = output[i]! ^ tweaks[i]!;
  return output;
}
