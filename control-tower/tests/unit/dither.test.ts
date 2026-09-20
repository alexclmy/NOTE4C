import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW } from "@/core/palette";
import {
  accentBudget,
  brushInk,
  cellFor,
  countIsolatedAccents,
  fillBandDither,
  fillDiscDither,
  fillRectDither,
  scrubIsolatedAccents,
} from "@/core/render/dither";
import { BRUSHES, PIXEL_TEXTURES, type Brush, type PixelTexture } from "@/core/theme";

const ALL_STYLES: Array<{ brush: Brush; texture: PixelTexture }> = BRUSHES.flatMap(
  (brush) => PIXEL_TEXTURES.map((texture) => ({ brush, texture })),
);

describe("the 2 px accent law", () => {
  it("never sizes a red or yellow cell below 2 px, at any texture", () => {
    for (const texture of PIXEL_TEXTURES) {
      expect(cellFor(RED, texture)).toBeGreaterThanOrEqual(2);
      expect(cellFor(YELLOW, texture)).toBeGreaterThanOrEqual(2);
    }
  });

  it("lets black and white go to a single pixel at the finest texture", () => {
    expect(cellFor(BLACK, "fine")).toBe(1);
    expect(cellFor(WHITE, "fine")).toBe(1);
  });

  it("leaves no isolated red or yellow speck in a flat field, any brush/texture/tone", () => {
    for (const style of ALL_STYLES) {
      for (const pigment of [RED, YELLOW] as const) {
        for (const tone of [0.05, 0.2, 0.4, 0.6, 0.85]) {
          const fb = new FrameBuffer(WHITE);
          fillRectDither(fb, { x: 3, y: 5, w: 191, h: 143 }, pigment, tone, style);
          expect(countIsolatedAccents(fb)).toBe(0);
          // And it still packs: only palette indices reached the buffer.
          expect(() => pack(fb)).not.toThrow();
        }
      }
    }
  });

  it("leaves no isolated accent on a curved mask (the sun disc), any style", () => {
    for (const style of ALL_STYLES) {
      const fb = new FrameBuffer(WHITE);
      fillDiscDither(fb, 200, 150, 31, RED, 0.8, style, { edgeSoftness: 0.4 });
      expect(countIsolatedAccents(fb)).toBe(0);
      expect(() => pack(fb)).not.toThrow();
    }
  });

  it("leaves no isolated accent on a tonal band gradient, any style", () => {
    for (const style of ALL_STYLES) {
      const fb = new FrameBuffer(WHITE);
      fillBandDither(fb, { x: 0, y: 0, w: 400, h: 60 }, YELLOW, {
        from: 0.9,
        to: 0.02,
        axis: "y",
        style,
      });
      expect(countIsolatedAccents(fb)).toBe(0);
    }
  });

  it("scrubs a planted isolated accent but keeps a connected pair", () => {
    const fb = new FrameBuffer(WHITE);
    fb.set(10, 10, RED); // lone speck
    fb.set(20, 20, YELLOW); // a connected pair survives
    fb.set(21, 20, YELLOW);
    scrubIsolatedAccents(fb, { x: 0, y: 0, w: 400, h: 300 });
    expect(fb.get(10, 10)).toBe(WHITE);
    expect(fb.get(20, 20)).toBe(YELLOW);
    expect(fb.get(21, 20)).toBe(YELLOW);
  });
});

describe("ordered brushes", () => {
  it("are deterministic: identical inputs, identical bytes", () => {
    const render = (): Uint8Array => {
      const fb = new FrameBuffer(WHITE);
      fillRectDither(fb, { x: 0, y: 0, w: 200, h: 150 }, RED, 0.5, {
        brush: "halftone",
        texture: "medium",
      });
      return pack(fb);
    };
    expect(Array.from(render())).toEqual(Array.from(render()));
  });

  it("ink monotonically more as the tone rises, for grain and halftone", () => {
    for (const brush of ["grain", "halftone"] as const) {
      const coverage = (tone: number): number => {
        let inked = 0;
        for (let cy = 0; cy < 8; cy += 1) {
          for (let cx = 0; cx < 8; cx += 1) {
            if (brushInk(brush, cx, cy, tone)) inked += 1;
          }
        }
        return inked;
      };
      expect(coverage(0.25)).toBeLessThan(coverage(0.5));
      expect(coverage(0.5)).toBeLessThan(coverage(0.75));
    }
  });

  it("draws nothing at tone 0 and fills solid at tone 1", () => {
    for (const brush of BRUSHES) {
      expect(brushInk(brush, 3, 4, 0)).toBe(false);
      expect(brushInk(brush, 3, 4, 1)).toBe(true);
    }
  });

  it("grid reads as a crosshatch near 0.4 coverage, not empty and not solid", () => {
    let inked = 0;
    for (let cy = 0; cy < 8; cy += 1) {
      for (let cx = 0; cx < 8; cx += 1) {
        if (brushInk("grid", cx, cy, 0.4)) inked += 1;
      }
    }
    expect(inked).toBeGreaterThan(8);
    expect(inked).toBeLessThan(56);
  });
});

describe("accent budget", () => {
  it("spends nothing in black & white and reports it", () => {
    expect(accentBudget("blackwhite")).toMatchObject({ usesAccent: false, scale: 0 });
  });

  it("spends more, but never a flat solid, in expressive than balanced", () => {
    expect(accentBudget("expressive").ceiling).toBeGreaterThan(
      accentBudget("balanced").ceiling,
    );
    expect(accentBudget("expressive").ceiling).toBeLessThan(1);
  });
});
