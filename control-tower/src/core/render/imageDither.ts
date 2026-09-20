import {
  BLACK,
  PREVIEW_RGB_TRIPLETS,
  RED,
  WHITE,
  YELLOW,
  type PaletteIndex,
} from "@/core/palette";

/**
 * Image dithering to the device's four-colour palette.
 *
 * WHY THIS LIVES IN CORE AND IS PURE
 * ----------------------------------
 * The browser inspector runs these functions on a resized `<canvas>` to turn a
 * photo into palette indices, and the same functions back the unit tests. They
 * touch no DOM and pull no native decoder, so the renderer stays free of
 * `sharp` and the server never has to decode an image: the browser dithers
 * once, the RESULT (indices) is stored with the module, and every later render
 * just blits those indices. That is what keeps the push pipeline deterministic
 * — there is no `Math.random` here, ordered dithering uses a fixed Bayer
 * matrix, so the same source and options always produce the same bytes.
 *
 * THE 2 PX ACCENT RULE
 * --------------------
 * The panel develops four pigments over tens of seconds and renders a lone red
 * or yellow pixel poorly — it reads as noise, not colour. So after quantising,
 * `scrubIsolatedAccents` removes every accent pixel that has no same-colour
 * neighbour, iterating until none remain. The stored tile therefore never
 * carries an isolated accent, and `countIsolatedAccents` lets a test assert it.
 */

/** Luma weights (Rec. 601). The eye weighs green far above blue. */
const LUMA_R = 0.299;
const LUMA_G = 0.587;
const LUMA_B = 0.114;

export interface RGB {
  r: number;
  g: number;
  b: number;
}

const PALETTE_RGB: Record<PaletteIndex, RGB> = {
  [BLACK]: triplet(BLACK),
  [WHITE]: triplet(WHITE),
  [YELLOW]: triplet(YELLOW),
  [RED]: triplet(RED),
};

function triplet(index: PaletteIndex): RGB {
  const [r, g, b] = PREVIEW_RGB_TRIPLETS[index] as readonly [
    number,
    number,
    number,
  ];
  return { r, g, b };
}

/** The four palette indices in the order the panel names them. */
const PALETTE_ORDER: readonly PaletteIndex[] = [BLACK, WHITE, YELLOW, RED];

/** Which indices are accents — the pigments the 2 px rule protects. */
const ACCENTS: ReadonlySet<number> = new Set<number>([YELLOW, RED]);

export type DitherMode = "photo" | "poster";

export interface DitherOptions {
  mode: DitherMode;
  /**
   * B&W (0) to expressive (100). It scales the source's chroma before
   * quantising: at 0 the image is greyscale, so only black and white can be
   * chosen; at 100 the chroma is doubled, so reds and yellows survive
   * quantisation instead of collapsing to grey.
   */
  colourAmount: number;
  /** Pre-boost contrast, -100 (flat) to 100 (hard). 0 leaves it untouched. */
  contrast: number;
}

export const DEFAULT_DITHER_OPTIONS: DitherOptions = {
  mode: "photo",
  colourAmount: 60,
  contrast: 0,
};

/** Luma-weighted squared distance between two colours. */
function distanceSq(a: RGB, b: RGB): number {
  const dr = a.r - b.r;
  const dg = a.g - b.g;
  const db = a.b - b.b;
  return LUMA_R * dr * dr + LUMA_G * dg * dg + LUMA_B * db * db;
}

/** The nearest palette index to a colour, by luma-weighted distance. */
export function nearestPaletteIndex(colour: RGB): PaletteIndex {
  let best: PaletteIndex = WHITE;
  let bestDist = Infinity;
  for (const index of PALETTE_ORDER) {
    const dist = distanceSq(colour, PALETTE_RGB[index]);
    if (dist < bestDist) {
      bestDist = dist;
      best = index;
    }
  }
  return best;
}

function clamp255(value: number): number {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return value;
}

/**
 * Apply the pre-boost the options ask for, in place on an RGB triple.
 *
 * Contrast first (a standard pivot-around-128 curve), then chroma scaling
 * around the pixel's own luma. Chroma scale is `colourAmount / 50`, so 50 is
 * neutral, 0 is greyscale and 100 doubles saturation.
 */
export function preprocess(colour: RGB, options: DitherOptions): RGB {
  let { r, g, b } = colour;

  if (options.contrast !== 0) {
    const c = Math.max(-255, Math.min(255, (options.contrast / 100) * 128));
    const factor = (259 * (c + 255)) / (255 * (259 - c));
    r = clamp255(factor * (r - 128) + 128);
    g = clamp255(factor * (g - 128) + 128);
    b = clamp255(factor * (b - 128) + 128);
  }

  const chroma = options.colourAmount / 50;
  if (chroma !== 1) {
    const luma = LUMA_R * r + LUMA_G * g + LUMA_B * b;
    r = clamp255(luma + (r - luma) * chroma);
    g = clamp255(luma + (g - luma) * chroma);
    b = clamp255(luma + (b - luma) * chroma);
  }

  return { r, g, b };
}

/** The classic 8x8 Bayer matrix, normalised to a -0.5..0.5 bias. */
const BAYER_8 = buildBayer8();

function buildBayer8(): number[] {
  // Recursive Bayer construction, then normalise to [-0.5, 0.5).
  const base = [
    [0, 48, 12, 60, 3, 51, 15, 63],
    [32, 16, 44, 28, 35, 19, 47, 31],
    [8, 56, 4, 52, 11, 59, 7, 55],
    [40, 24, 36, 20, 43, 27, 39, 23],
    [2, 50, 14, 62, 1, 49, 13, 61],
    [34, 18, 46, 30, 33, 17, 45, 29],
    [10, 58, 6, 54, 9, 57, 5, 53],
    [42, 26, 38, 22, 41, 25, 37, 21],
  ];
  const out: number[] = [];
  for (let y = 0; y < 8; y += 1) {
    for (let x = 0; x < 8; x += 1) {
      out[y * 8 + x] = ((base[y] as number[])[x] as number) / 64 - 0.5;
    }
  }
  return out;
}

/** Read one pixel out of an RGBA buffer. */
function readPixel(rgba: ArrayLike<number>, offset: number): RGB {
  return {
    r: rgba[offset] as number,
    g: rgba[offset + 1] as number,
    b: rgba[offset + 2] as number,
  };
}

/**
 * Floyd-Steinberg error diffusion — the "Photo" recipe. Errors spread with the
 * canonical 7/16, 3/16, 5/16, 1/16 kernel, in raster order, so the result is a
 * pure function of the input.
 */
export function floydSteinberg(
  rgba: ArrayLike<number>,
  width: number,
  height: number,
  options: DitherOptions,
): Uint8Array {
  const out = new Uint8Array(width * height);
  // A float working buffer holds the pre-processed image plus diffused error.
  const buf = new Float32Array(width * height * 3);
  for (let i = 0; i < width * height; i += 1) {
    const pre = preprocess(readPixel(rgba, i * 4), options);
    buf[i * 3] = pre.r;
    buf[i * 3 + 1] = pre.g;
    buf[i * 3 + 2] = pre.b;
  }

  const spread = (i: number, er: number, eg: number, eb: number, w: number) => {
    buf[i * 3] = (buf[i * 3] as number) + er * w;
    buf[i * 3 + 1] = (buf[i * 3 + 1] as number) + eg * w;
    buf[i * 3 + 2] = (buf[i * 3 + 2] as number) + eb * w;
  };

  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const i = y * width + x;
      const old: RGB = {
        r: buf[i * 3] as number,
        g: buf[i * 3 + 1] as number,
        b: buf[i * 3 + 2] as number,
      };
      const index = nearestPaletteIndex(old);
      out[i] = index;
      const chosen = PALETTE_RGB[index];
      const er = old.r - chosen.r;
      const eg = old.g - chosen.g;
      const eb = old.b - chosen.b;
      if (x + 1 < width) spread(i + 1, er, eg, eb, 7 / 16);
      if (y + 1 < height) {
        if (x > 0) spread(i + width - 1, er, eg, eb, 3 / 16);
        spread(i + width, er, eg, eb, 5 / 16);
        if (x + 1 < width) spread(i + width + 1, er, eg, eb, 1 / 16);
      }
    }
  }
  return out;
}

/**
 * Ordered (Bayer) dithering — the "Poster" recipe. A fixed 8x8 threshold gives
 * flat, print-like fields with no error trails, which suits graphic images.
 */
export function orderedDither(
  rgba: ArrayLike<number>,
  width: number,
  height: number,
  options: DitherOptions,
): Uint8Array {
  const out = new Uint8Array(width * height);
  // Bias amplitude in 0..255 space; a quarter step reads as a clean screen.
  const amplitude = 64;
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const i = y * width + x;
      const pre = preprocess(readPixel(rgba, i * 4), options);
      const bias = (BAYER_8[(y & 7) * 8 + (x & 7)] as number) * amplitude;
      out[i] = nearestPaletteIndex({
        r: clamp255(pre.r + bias),
        g: clamp255(pre.g + bias),
        b: clamp255(pre.b + bias),
      });
    }
  }
  return out;
}

/**
 * Remove isolated accent pixels so the 2 px rule holds.
 *
 * An accent pixel (yellow or red) with no orthogonally- or diagonally-adjacent
 * pixel of the SAME accent is demoted to black or white — whichever its own
 * colour is nearer to — because the panel cannot render a single coloured
 * speck cleanly. Demotion only ever removes accents, so iterating to a fixed
 * point terminates, and afterwards no isolated accent remains.
 */
export function scrubIsolatedAccents(
  indices: Uint8Array,
  width: number,
  height: number,
): Uint8Array {
  let changed = true;
  while (changed) {
    changed = false;
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) {
        const i = y * width + x;
        const value = indices[i] as number;
        if (!ACCENTS.has(value)) continue;
        if (hasSameAccentNeighbour(indices, width, height, x, y, value)) continue;
        // Demote to the nearer neutral. Red is dark, yellow is light.
        const neutral: PaletteIndex =
          distanceSq(PALETTE_RGB[value as PaletteIndex], PALETTE_RGB[BLACK]) <=
          distanceSq(PALETTE_RGB[value as PaletteIndex], PALETTE_RGB[WHITE])
            ? BLACK
            : WHITE;
        indices[i] = neutral;
        changed = true;
      }
    }
  }
  return indices;
}

function hasSameAccentNeighbour(
  indices: Uint8Array,
  width: number,
  height: number,
  x: number,
  y: number,
  accent: number,
): boolean {
  for (let dy = -1; dy <= 1; dy += 1) {
    for (let dx = -1; dx <= 1; dx += 1) {
      if (dx === 0 && dy === 0) continue;
      const nx = x + dx;
      const ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
      if ((indices[ny * width + nx] as number) === accent) return true;
    }
  }
  return false;
}

/** How many isolated accent pixels remain. Zero is the contract. */
export function countIsolatedAccents(
  indices: Uint8Array,
  width: number,
  height: number,
): number {
  let count = 0;
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const value = indices[y * width + x] as number;
      if (!ACCENTS.has(value)) continue;
      if (!hasSameAccentNeighbour(indices, width, height, x, y, value)) {
        count += 1;
      }
    }
  }
  return count;
}

/**
 * Dither an RGBA tile to palette indices, mode chosen by options, with the
 * 2 px accent rule enforced. This is the one entry point the inspector calls.
 */
export function ditherImage(
  rgba: ArrayLike<number>,
  width: number,
  height: number,
  options: DitherOptions,
): Uint8Array {
  const indices =
    options.mode === "poster"
      ? orderedDither(rgba, width, height, options)
      : floydSteinberg(rgba, width, height, options);
  return scrubIsolatedAccents(indices, width, height);
}

// --------------------------------------------------------------- storage ----

const B64 =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

function base64Encode(bytes: Uint8Array): string {
  let out = "";
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i] as number;
    const b1 = i + 1 < bytes.length ? (bytes[i + 1] as number) : 0;
    const b2 = i + 2 < bytes.length ? (bytes[i + 2] as number) : 0;
    out += B64[b0 >> 2];
    out += B64[((b0 & 3) << 4) | (b1 >> 4)];
    out += i + 1 < bytes.length ? B64[((b1 & 15) << 2) | (b2 >> 6)] : "=";
    out += i + 2 < bytes.length ? B64[b2 & 63] : "=";
  }
  return out;
}

function base64Decode(text: string): Uint8Array {
  const clean = text.replace(/[^A-Za-z0-9+/]/g, "");
  const len = Math.floor((clean.length * 3) / 4);
  const out = new Uint8Array(len);
  let outPos = 0;
  for (let i = 0; i < clean.length; i += 4) {
    const c0 = B64.indexOf(clean[i] as string);
    const c1 = B64.indexOf(clean[i + 1] as string);
    const c2 = i + 2 < clean.length ? B64.indexOf(clean[i + 2] as string) : -1;
    const c3 = i + 3 < clean.length ? B64.indexOf(clean[i + 3] as string) : -1;
    if (outPos < len) out[outPos++] = (c0 << 2) | (c1 >> 4);
    if (c2 >= 0 && outPos < len) out[outPos++] = ((c1 & 15) << 4) | (c2 >> 2);
    if (c3 >= 0 && outPos < len) out[outPos++] = ((c2 & 3) << 6) | c3;
  }
  return out;
}

/**
 * Pack a tile of palette indices to 2 bits per pixel, MSB first, four pixels
 * per byte — the device's own frame layout, but for a sub-rectangle — then
 * base64 it. The result is bounded: a full 400x300 tile is 30000 bytes, about
 * 40000 base64 characters, and a typical tile is far smaller.
 */
export function encodeTile(
  indices: Uint8Array,
  width: number,
  height: number,
): string {
  const count = width * height;
  const bytes = new Uint8Array(Math.ceil(count / 4));
  for (let i = 0; i < count; i += 1) {
    const value = (indices[i] as number) & 3;
    const byteIndex = i >> 2;
    bytes[byteIndex] = (bytes[byteIndex] as number) | (value << (6 - 2 * (i & 3)));
  }
  return base64Encode(bytes);
}

/** Inverse of {@link encodeTile}: base64 2bpp back to one index per pixel. */
export function decodeTile(
  data: string,
  width: number,
  height: number,
): Uint8Array {
  const count = width * height;
  const bytes = base64Decode(data);
  const out = new Uint8Array(count);
  for (let i = 0; i < count; i += 1) {
    const byte = bytes[i >> 2] as number;
    out[i] = ((byte ?? 0) >> (6 - 2 * (i & 3))) & 3;
  }
  return out;
}
