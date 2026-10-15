/** Small baseline JPEG encoder. 4:4:4 color, 8-bit DCT, no native runtime dependencies. */
const zigzag = [
  0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
  13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52,
  45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
];
const basis = Array.from({ length: 8 }, (_, u) =>
  Array.from(
    { length: 8 },
    (_, x) => 0.5 * (u === 0 ? Math.SQRT1_2 : 1) * Math.cos(((2 * x + 1) * u * Math.PI) / 16),
  ),
);
const acSymbols = [
  0,
  0xf0,
  ...Array.from({ length: 160 }, (_, i) => (Math.floor(i / 10) << 4) | ((i % 10) + 1)),
];
const acCodes = new Map(acSymbols.map((symbol, code) => [symbol, code]));

export function encodeAvatarJpeg(rgba: Uint8Array): Uint8Array {
  if (rgba.length !== 256 * 256 * 4) throw new Error("Expected 256x256 RGBA");
  // acc::LoadImage truncates at 0x20000 bytes. Reduce detail instead of emitting a JPEG
  // whose entropy stream the guest would receive cut in half.
  for (const quantizer of [12, 24, 48, 96, 192, 255]) {
    const jpeg = encodeJpeg(rgba, quantizer);
    if (jpeg.length <= 0x20000) return jpeg;
  }
  throw new Error("Avatar exceeds the guest JPEG limit");
}

function encodeJpeg(rgba: Uint8Array, quantizer: number): Uint8Array {
  if (rgba.length !== 256 * 256 * 4) throw new Error("Expected 256x256 RGBA");
  const out: number[] = [];
  const byte = (value: number) => {
    out.push(value & 255);
  };
  const word = (value: number) => {
    byte(value >> 8);
    byte(value);
  };
  const marker = (id: number, data: number[]) => {
    byte(255);
    byte(id);
    word(data.length + 2);
    data.forEach(byte);
  };
  byte(255);
  byte(0xd8);
  marker(0xdb, [0, ...Array(64).fill(quantizer)]);
  marker(0xc0, [8, 1, 0, 1, 0, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0]);
  marker(0xc4, [
    0,
    ...Array.from({ length: 16 }, (_, i) => (i === 3 ? 12 : 0)),
    ...Array.from({ length: 12 }, (_, i) => i),
    0x10,
    ...Array.from({ length: 16 }, (_, i) => (i === 7 ? acSymbols.length : 0)),
    ...acSymbols,
  ]);
  marker(0xda, [3, 1, 0, 2, 0, 3, 0, 0, 63, 0]);
  let pending = 0,
    count = 0;
  const bits = (value: number, length: number) => {
    for (let i = length - 1; i >= 0; --i) {
      pending = (pending << 1) | ((value >> i) & 1);
      if (++count === 8) {
        byte(pending);
        if ((pending & 255) === 255) byte(0);
        pending = 0;
        count = 0;
      }
    }
  };
  const magnitude = (value: number) =>
    value === 0 ? 0 : Math.floor(Math.log2(Math.abs(value))) + 1;
  const amplitude = (value: number, length: number) =>
    bits(value < 0 ? value + (1 << length) - 1 : value, length);
  const previous = [0, 0, 0];
  const samples = new Float64Array(64),
    horizontal = new Float64Array(64),
    coefficients = new Int32Array(64);
  for (let by = 0; by < 256; by += 8)
    for (let bx = 0; bx < 256; bx += 8)
      for (let component = 0; component < 3; ++component) {
        for (let y = 0; y < 8; ++y)
          for (let x = 0; x < 8; ++x) {
            const p = ((by + y) * 256 + bx + x) * 4;
            const alpha = rgba[p + 3]! / 255;
            const r = rgba[p]! * alpha,
              g = rgba[p + 1]! * alpha,
              b = rgba[p + 2]! * alpha;
            samples[y * 8 + x] =
              component === 0
                ? 0.299 * r + 0.587 * g + 0.114 * b - 128
                : component === 1
                  ? -0.168736 * r - 0.331264 * g + 0.5 * b
                  : 0.5 * r - 0.418688 * g - 0.081312 * b;
          }
        for (let y = 0; y < 8; ++y)
          for (let u = 0; u < 8; ++u) {
            let sum = 0;
            for (let x = 0; x < 8; ++x) sum += samples[y * 8 + x]! * basis[u]![x]!;
            horizontal[y * 8 + u] = sum;
          }
        for (let v = 0; v < 8; ++v)
          for (let u = 0; u < 8; ++u) {
            let sum = 0;
            for (let y = 0; y < 8; ++y) sum += horizontal[y * 8 + u]! * basis[v]![y]!;
            coefficients[v * 8 + u] = Math.round(sum / quantizer);
          }
        const delta = coefficients[0]! - previous[component]!;
        previous[component] = coefficients[0]!;
        const length = magnitude(delta);
        bits(length, 4);
        amplitude(delta, length);
        let zeros = 0;
        for (let i = 1; i < 64; ++i) {
          const value = coefficients[zigzag[i]!]!;
          if (!value) {
            ++zeros;
            continue;
          }
          while (zeros >= 16) {
            bits(acCodes.get(0xf0)!, 8);
            zeros -= 16;
          }
          const size = magnitude(value);
          bits(acCodes.get((zeros << 4) | size)!, 8);
          amplitude(value, size);
          zeros = 0;
        }
        if (zeros) bits(acCodes.get(0)!, 8);
      }
  if (count) bits((1 << (8 - count)) - 1, 8 - count);
  byte(255);
  byte(0xd9);
  return Uint8Array.from(out);
}
