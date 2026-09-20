#!/usr/bin/env node
/**
 * InkScreen image converter for NAS / local push services.
 *
 * What the device /upload API expects:
 * - 400x300 1bpp black/white raw buffer: 15000 bytes
 * - 400x300 2bpp black/white/yellow/red raw buffer: 30000 bytes
 *
 * The algorithm matches the firmware HTML uploader:
 * 1. Decode the source image to RGBA.
 * 2. Resize/pad to exactly 400x300 on a white background.
 * 3. Apply Floyd-Steinberg dithering.
 * 4. Pack pixels into the raw format expected by /upload.
 *
 * This file has no dependency for the core conversion functions. The optional
 * CLI uses `sharp` only for image decode/resize:
 *
 *   npm install sharp
 *   node inkscreen_image_converter.js input.jpg output.bin bwry2bpp
 *   curl -X POST "http://DEVICE_IP/upload?format=bwry2bpp" \
 *     -H "Content-Type: application/octet-stream" \
 *     --data-binary "@output.bin"
 */

const SCREEN_WIDTH = 400;
const SCREEN_HEIGHT = 300;
const PIXELS = SCREEN_WIDTH * SCREEN_HEIGHT;
const SIZE_1BPP = PIXELS / 8;
const SIZE_2BPP = PIXELS * 2 / 8;

/**
 * Convert a 400x300 RGBA buffer to 1bpp black/white raw data.
 *
 * Output packing:
 * - 8 pixels per byte.
 * - pixel 0 uses bit 7, pixel 7 uses bit 0.
 * - bit 1 = white, bit 0 = black.
 */
function rgbaTo1bpp(rgba, width = SCREEN_WIDTH, height = SCREEN_HEIGHT) {
  assertRgba(rgba, width, height);

  const gray = new Int16Array(width * height);
  for (let p = 0, i = 0; p < gray.length; p += 1, i += 4) {
    // Same integer luminance approximation as the HTML uploader.
    gray[p] = ((rgba[i] * 30 + rgba[i + 1] * 59 + rgba[i + 2] * 11) / 100) | 0;
  }

  const out = new Uint8Array(width * height / 8);

  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const p = y * width + x;
      const old = clamp(gray[p], 0, 255);
      const next = old > 128 ? 255 : 0;
      const err = old - next;

      if (next > 128) {
        out[p >> 3] |= 1 << (7 - (p & 7));
      }

      diffuseGray(gray, width, height, x, y, err);
    }
  }

  return out;
}

/**
 * Convert a 400x300 RGBA buffer to 2bpp black/white/yellow/red raw data.
 *
 * Palette selection uses weighted RGB distance:
 * - black:  [0, 0, 0]
 * - white:  [255, 255, 255]
 * - red:    [255, 0, 0]
 * - yellow: [255, 255, 0]
 *
 * Output packing:
 * - 4 pixels per byte.
 * - pixel 0 uses bits 7..6, pixel 3 uses bits 1..0.
 * - device color codes: black=0, white=1, yellow=2, red=3.
 */
function rgbaToBwry2bpp(rgba, width = SCREEN_WIDTH, height = SCREEN_HEIGHT) {
  assertRgba(rgba, width, height);

  const work = new Array(height);
  for (let y = 0; y < height; y += 1) {
    work[y] = new Array(width);
    for (let x = 0; x < width; x += 1) {
      const i = (y * width + x) * 4;
      work[y][x] = { r: rgba[i], g: rgba[i + 1], b: rgba[i + 2] };
    }
  }

  const palette = [
    [0, 0, 0],
    [255, 255, 255],
    [255, 0, 0],
    [255, 255, 0],
  ];

  const out = new Uint8Array(width * height * 2 / 8);

  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const old = work[y][x];
      let minDistance = Number.POSITIVE_INFINITY;
      let paletteIndex = 0;
      let selected = palette[0];

      for (let k = 0; k < palette.length; k += 1) {
        const candidate = palette[k];
        const distance =
          0.299 * (old.r - candidate[0]) ** 2 +
          0.587 * (old.g - candidate[1]) ** 2 +
          0.114 * (old.b - candidate[2]) ** 2;
        if (distance < minDistance) {
          minDistance = distance;
          paletteIndex = k;
          selected = candidate;
        }
      }

      // Palette index order above is black, white, red, yellow. The device
      // raw format uses black=0, white=1, yellow=2, red=3.
      const deviceColor = paletteIndex === 0 ? 0 :
        paletteIndex === 1 ? 1 :
        paletteIndex === 2 ? 3 : 2;

      const p = y * width + x;
      out[p >> 2] |= deviceColor << (6 - ((p & 3) * 2));

      const errR = old.r - selected[0];
      const errG = old.g - selected[1];
      const errB = old.b - selected[2];
      diffuseRgb(work, width, height, x, y, errR, errG, errB);
    }
  }

  return out;
}

function assertRgba(rgba, width, height) {
  if (!rgba || typeof rgba.length !== "number") {
    throw new Error("rgba must be a Uint8Array/Buffer-like object");
  }
  if (rgba.length !== width * height * 4) {
    throw new Error(`rgba length must be ${width * height * 4}, got ${rgba.length}`);
  }
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function diffuseGray(gray, width, height, x, y, err) {
  const p = y * width + x;
  if (x + 1 < width) gray[p + 1] += err * 7 / 16;
  if (y + 1 < height) {
    if (x > 0) gray[p + width - 1] += err * 3 / 16;
    gray[p + width] += err * 5 / 16;
    if (x + 1 < width) gray[p + width + 1] += err / 16;
  }
}

function diffuseRgb(work, width, height, x, y, errR, errG, errB) {
  const add = (dy, dx, factor) => {
    const ny = y + dy;
    const nx = x + dx;
    if (ny < 0 || ny >= height || nx < 0 || nx >= width) return;
    work[ny][nx].r += errR * factor;
    work[ny][nx].g += errG * factor;
    work[ny][nx].b += errB * factor;
  };

  add(0, 1, 7 / 16);
  add(1, -1, 3 / 16);
  add(1, 0, 5 / 16);
  add(1, 1, 1 / 16);
}

async function loadImageRgbaWithSharp(inputPath) {
  let sharp;
  try {
    sharp = require("sharp");
  } catch (err) {
    throw new Error("CLI mode requires sharp. Run: npm install sharp");
  }

  const { data } = await sharp(inputPath)
    .resize(SCREEN_WIDTH, SCREEN_HEIGHT, {
      fit: "contain",
      background: { r: 255, g: 255, b: 255, alpha: 1 },
    })
    .ensureAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });

  return data;
}

async function cli() {
  const fs = require("fs/promises");
  const [input, output, format = "bwry2bpp"] = process.argv.slice(2);

  if (!input || !output) {
    console.error("Usage: node inkscreen_image_converter.js <input-image> <output.bin> [bwry2bpp|1bpp]");
    process.exit(2);
  }

  const rgba = await loadImageRgbaWithSharp(input);
  const bin = format === "1bpp" ? rgbaTo1bpp(rgba) : rgbaToBwry2bpp(rgba);
  await fs.writeFile(output, bin);

  const expected = format === "1bpp" ? SIZE_1BPP : SIZE_2BPP;
  console.log(`Wrote ${output} (${bin.length} bytes, expected ${expected}, format=${format})`);
}

if (typeof module !== "undefined") {
  module.exports = {
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    SIZE_1BPP,
    SIZE_2BPP,
    rgbaTo1bpp,
    rgbaToBwry2bpp,
    loadImageRgbaWithSharp,
  };
}

if (require.main === module) {
  cli().catch((err) => {
    console.error(err.message || err);
    process.exit(1);
  });
}
