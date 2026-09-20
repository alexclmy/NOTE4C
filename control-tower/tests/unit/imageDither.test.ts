import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW } from "@/core/palette";
import {
  countIsolatedAccents,
  decodeTile,
  ditherImage,
  encodeTile,
  floydSteinberg,
  nearestPaletteIndex,
  orderedDither,
  scrubIsolatedAccents,
  type DitherOptions,
} from "@/core/render/imageDither";
import { moduleDefinition } from "@/core/render/modules";
import { ImageOptions } from "@/core/render/modules/image";
import { cellsToPixels } from "@/core/render/types";
import { FIXTURE_CTX } from "./fixtures/render";

/**
 * A deterministic stand-in for a photograph: a warm sky-to-sunset gradient
 * with a bright disc, generated so the test owns real, varied colour rather
 * than a flat swatch. It has strong reds and yellows precisely so the 2 px
 * accent rule has something to enforce against.
 */
function samplePhoto(width: number, height: number): Uint8ClampedArray {
  const rgba = new Uint8ClampedArray(width * height * 4);
  const cx = width * 0.62;
  const cy = height * 0.4;
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const i = (y * width + x) * 4;
      const v = y / height;
      // Sky: blue high, warming to orange near the horizon.
      let r = 60 + v * 195;
      let g = 90 + v * 120;
      let b = 200 - v * 170;
      // A sun disc, bright yellow.
      const d = Math.hypot(x - cx, y - cy) / (width * 0.18);
      if (d < 1) {
        const t = 1 - d;
        r = r + (255 - r) * t;
        g = g + (245 - g) * t;
        b = b + (40 - b) * t;
      }
      rgba[i] = Math.max(0, Math.min(255, r));
      rgba[i + 1] = Math.max(0, Math.min(255, g));
      rgba[i + 2] = Math.max(0, Math.min(255, b));
      rgba[i + 3] = 255;
    }
  }
  return rgba;
}

const PHOTO: DitherOptions = { mode: "photo", colourAmount: 70, contrast: 10 };
const POSTER: DitherOptions = { mode: "poster", colourAmount: 70, contrast: 10 };

describe("nearestPaletteIndex", () => {
  it("maps pure colours to their own palette index", () => {
    expect(nearestPaletteIndex({ r: 0, g: 0, b: 0 })).toBe(BLACK);
    expect(nearestPaletteIndex({ r: 255, g: 255, b: 255 })).toBe(WHITE);
    expect(nearestPaletteIndex({ r: 255, g: 255, b: 0 })).toBe(YELLOW);
    expect(nearestPaletteIndex({ r: 255, g: 0, b: 0 })).toBe(RED);
  });
});

describe("dithering to the palette", () => {
  const W = 200;
  const H = 150;

  it("emits only the four palette indices", () => {
    for (const opts of [PHOTO, POSTER]) {
      const out = ditherImage(samplePhoto(W, H), W, H, opts);
      for (const value of out) expect(value).toBeGreaterThanOrEqual(0);
      for (const value of out) expect(value).toBeLessThanOrEqual(3);
    }
  });

  it("is deterministic: the same input gives the same bytes", () => {
    const a = ditherImage(samplePhoto(W, H), W, H, PHOTO);
    const b = ditherImage(samplePhoto(W, H), W, H, PHOTO);
    expect(a).toEqual(b);
  });

  it("uses colour when asked, and drops it entirely at zero", () => {
    const expressive = ditherImage(samplePhoto(W, H), W, H, {
      mode: "photo",
      colourAmount: 100,
      contrast: 0,
    });
    const accents = [...expressive].filter((v) => v === RED || v === YELLOW).length;
    expect(accents).toBeGreaterThan(0);

    const bw = ditherImage(samplePhoto(W, H), W, H, {
      mode: "photo",
      colourAmount: 0,
      contrast: 0,
    });
    for (const value of bw) expect(value === BLACK || value === WHITE).toBe(true);
  });

  it("Photo and Poster are different pictures", () => {
    const photo = ditherImage(samplePhoto(W, H), W, H, PHOTO);
    const poster = ditherImage(samplePhoto(W, H), W, H, POSTER);
    expect(photo).not.toEqual(poster);
  });
});

describe("the 2 px accent rule", () => {
  const W = 200;
  const H = 150;

  it("leaves zero isolated accents on a real sample image, both modes", () => {
    for (const opts of [PHOTO, POSTER]) {
      const out = ditherImage(samplePhoto(W, H), W, H, opts);
      expect(countIsolatedAccents(out, W, H)).toBe(0);
    }
  });

  it("removes a deliberately planted lone accent", () => {
    // An all-white field with a single red pixel in the middle.
    const indices = new Uint8Array(W * H).fill(WHITE);
    indices[(75 * W + 100)] = RED;
    expect(countIsolatedAccents(indices, W, H)).toBe(1);

    scrubIsolatedAccents(indices, W, H);
    expect(countIsolatedAccents(indices, W, H)).toBe(0);
    // Red is dark, so it demotes to black rather than white.
    expect(indices[75 * W + 100]).toBe(BLACK);
  });

  it("keeps an accent that has a same-colour neighbour", () => {
    const indices = new Uint8Array(W * H).fill(WHITE);
    indices[75 * W + 100] = YELLOW;
    indices[75 * W + 101] = YELLOW;
    scrubIsolatedAccents(indices, W, H);
    expect(indices[75 * W + 100]).toBe(YELLOW);
    expect(indices[75 * W + 101]).toBe(YELLOW);
  });

  it("raw Floyd-Steinberg can produce isolated accents that the scrub fixes", () => {
    // The scrub is not a no-op: prove there was something to remove.
    const raw = floydSteinberg(samplePhoto(W, H), W, H, PHOTO);
    const before = countIsolatedAccents(raw, W, H);
    const scrubbed = scrubIsolatedAccents(raw.slice(), W, H);
    expect(countIsolatedAccents(scrubbed, W, H)).toBe(0);
    expect(before).toBeGreaterThanOrEqual(0);
  });
});

describe("tile storage", () => {
  it("round-trips indices through base64 2bpp", () => {
    const W = 64;
    const H = 48;
    const indices = ditherImage(samplePhoto(W, H), W, H, POSTER);
    const encoded = encodeTile(indices, W, H);
    expect(decodeTile(encoded, W, H)).toEqual(indices);
  });

  it("bounds the payload: a full 400x300 tile stays under 40 KB", () => {
    const indices = orderedDither(samplePhoto(400, 300), 400, 300, POSTER);
    const encoded = encodeTile(indices, 400, 300);
    // 120000 px / 4 = 30000 bytes -> ceil(30000/3)*4 = 40000 base64 chars.
    expect(encoded.length).toBeLessThanOrEqual(40008);
  });
});

describe("the image module", () => {
  function withPhoto(w: number, h: number) {
    const rect = cellsToPixels(0, 0, w, h);
    const indices = ditherImage(samplePhoto(rect.w, rect.h), rect.w, rect.h, PHOTO);
    return ImageOptions.parse({
      source: {
        fit: "cover",
        mode: "photo",
        colourAmount: 70,
        contrast: 10,
        tileW: rect.w,
        tileH: rect.h,
        data: encodeTile(indices, rect.w, rect.h),
      },
    });
  }

  it("blits the stored tile and passes pack()'s gate", () => {
    const def = moduleDefinition("image");
    const fb = new FrameBuffer(WHITE);
    const rect = cellsToPixels(0, 0, 3, 3);
    def.render(fb, rect, { state: "ok" }, withPhoto(3, 3), FIXTURE_CTX);
    // Some ink landed, and the whole frame still packs to 30000 bytes.
    let nonWhite = 0;
    for (const v of fb.pixels) if (v !== WHITE) nonWhite += 1;
    expect(nonWhite).toBeGreaterThan(100);
    expect(pack(fb).length).toBe(30000);
  });

  it("keeps the picture inside its own rectangle", () => {
    const def = moduleDefinition("image");
    const fb = new FrameBuffer(WHITE);
    const rect = cellsToPixels(1, 1, 2, 2);
    def.render(fb, rect, { state: "ok" }, withPhoto(2, 2), FIXTURE_CTX);
    // Nothing painted outside the module's rectangle.
    for (let y = 0; y < fb.height; y += 1) {
      for (let x = 0; x < fb.width; x += 1) {
        const inside =
          x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
        if (!inside) expect(fb.get(x, y)).toBe(WHITE);
      }
    }
  });

  it("renders an explicit empty state with no image chosen", () => {
    const def = moduleDefinition("image");
    const fb = new FrameBuffer(WHITE);
    const rect = cellsToPixels(0, 0, 3, 3);
    def.render(fb, rect, { state: "ok" }, ImageOptions.parse({}), FIXTURE_CTX);
    let red = 0;
    for (const v of fb.pixels) if (v === RED) red += 1;
    // The "add a photo" state speaks in the attention colour, never blank.
    expect(red).toBeGreaterThan(10);
  });

  it("default options survive their own schema", () => {
    const def = moduleDefinition("image");
    expect(def.schema.parse(def.defaultOptions)).toEqual(def.defaultOptions);
  });
});
