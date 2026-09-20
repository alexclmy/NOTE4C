import { describe, expect, it } from "vitest";
import {
  FrameBuffer,
  NonPalettePixelError,
  pack,
  unpack,
} from "@/core/frame";
import {
  BLACK,
  FRAME_HEIGHT,
  FRAME_PIXELS,
  FRAME_WIDTH,
  PACKED_BYTES,
  RED,
  WHITE,
  YELLOW,
  isPaletteIndex,
} from "@/core/palette";
import { sha256HexSync } from "@/server/hash";
import { availableAtlases, font } from "@/core/render/fonts";
import {
  FONT_FAMILY_IDS,
  FONT_SIZES,
  FONT_WEIGHTS,
  fitText,
  measureText,
} from "@/core/font";

/**
 * The four-line sample above, packed. This pins the glyph pipeline end to
 * end: change a vendored face, a size on the ladder, or the rasteriser, and
 * this digest moves. It is expected to move when that is what was intended.
 */
const TEXT_SAMPLE_DIGEST =
  // Moved when the sample's first word was made generic. The sample is a
  // string chosen to exercise four faces and four sizes, not a fact about
  // anything, so its wording is free to change; the pipeline is what is pinned.
  "7f6b0abf1278923cf766221642dfd08b2c6b2f21f0df4d9ef6e48c5faf46a3bf";

describe("palette", () => {
  it("uses the device ordering, which is the panel contract", () => {
    expect([BLACK, WHITE, YELLOW, RED]).toEqual([0, 1, 2, 3]);
    expect(isPaletteIndex(3)).toBe(true);
    expect(isPaletteIndex(4)).toBe(false);
  });
});

describe("pack", () => {
  it("emits exactly 30000 bytes", () => {
    expect(pack(new FrameBuffer(WHITE)).length).toBe(PACKED_BYTES);
    expect(PACKED_BYTES).toBe(30000);
    expect(FRAME_PIXELS).toBe(120000);
  });

  it("packs four pixels per byte, MSB first", () => {
    const fb = new FrameBuffer(BLACK);
    // First four pixels: black, white, yellow, red -> 00 01 10 11 -> 0x1B.
    fb.set(0, 0, BLACK);
    fb.set(1, 0, WHITE);
    fb.set(2, 0, YELLOW);
    fb.set(3, 0, RED);
    const bytes = pack(fb);
    expect(bytes[0]).toBe(0b00011011);
    expect(bytes[0]).toBe(0x1b);
  });

  it("packs known 4-pixel patterns to known bytes", () => {
    const cases: Array<[number[], number]> = [
      [[0, 0, 0, 0], 0x00],
      [[1, 1, 1, 1], 0x55],
      [[2, 2, 2, 2], 0xaa],
      [[3, 3, 3, 3], 0xff],
      [[3, 0, 0, 0], 0xc0],
      [[0, 3, 0, 0], 0x30],
      [[0, 0, 3, 0], 0x0c],
      [[0, 0, 0, 3], 0x03],
      [[1, 2, 3, 0], 0x6c],
      [[2, 0, 1, 3], 0x87],
    ];
    for (const [quad, expected] of cases) {
      const fb = new FrameBuffer(BLACK);
      quad.forEach((value, index) => fb.set(index, 0, value as 0 | 1 | 2 | 3));
      expect(pack(fb)[0], `pattern ${quad.join(",")}`).toBe(expected);
    }
  });

  it("throws on any pixel outside the palette", () => {
    const fb = new FrameBuffer(WHITE);
    fb.pixels[54321] = 7;
    expect(() => pack(fb)).toThrow(NonPalettePixelError);
    try {
      pack(fb);
    } catch (error) {
      expect((error as NonPalettePixelError).offset).toBe(54321);
      expect((error as NonPalettePixelError).value).toBe(7);
    }
  });

  it("refuses a buffer of the wrong size", () => {
    expect(() => pack(new Uint8Array(1000))).toThrow(RangeError);
  });

  it("round-trips through unpack", () => {
    const fb = new FrameBuffer(WHITE);
    fb.fillRect(10, 10, 33, 21, RED);
    fb.fillRect(200, 150, 17, 9, YELLOW);
    fb.hline(0, 299, 400, BLACK);
    const restored = unpack(pack(fb));
    expect(restored.pixels).toEqual(fb.pixels);
  });

  it("produces stable golden digests for reference frames", () => {
    const allWhite = pack(new FrameBuffer(WHITE));
    expect(sha256HexSync(allWhite)).toBe(
      sha256HexSync(new Uint8Array(PACKED_BYTES).fill(0x55)),
    );

    const allBlack = pack(new FrameBuffer(BLACK));
    expect(new Set(allBlack)).toEqual(new Set([0x00]));

    // Left half red, right half white. Independently constructed rather than
    // asserted against an opaque digest: each row is 50 bytes of 0xFF (four
    // red pixels each) followed by 50 bytes of 0x55 (four white pixels each).
    const halves = new FrameBuffer(WHITE);
    halves.fillRect(0, 0, 200, 300, RED);
    const constructed = new Uint8Array(PACKED_BYTES);
    for (let row = 0; row < FRAME_HEIGHT; row += 1) {
      constructed.fill(0xff, row * 100, row * 100 + 50);
      constructed.fill(0x55, row * 100 + 50, row * 100 + 100);
    }
    expect(pack(halves)).toEqual(constructed);
    expect(sha256HexSync(pack(halves))).toBe(
      "68fc3992547d4889c994e0b85f8cb29d1066417a817fa3c16798bf0f12d79d5f",
    );
  });
});

describe("FrameBuffer primitives", () => {
  it("clips writes outside the canvas instead of throwing", () => {
    const fb = new FrameBuffer(WHITE);
    fb.set(-1, -1, RED);
    fb.set(FRAME_WIDTH, FRAME_HEIGHT, RED);
    fb.fillRect(-50, -50, 60, 60, RED);
    expect(fb.get(-1, 0)).toBe(-1);
    expect(fb.get(0, 0)).toBe(RED);
    expect(fb.get(10, 10)).toBe(WHITE);
    expect(pack(fb).length).toBe(PACKED_BYTES);
  });

  it("strokes a rect as a 1 px outline", () => {
    const fb = new FrameBuffer(WHITE);
    fb.strokeRect(5, 5, 10, 8, BLACK);
    expect(fb.get(5, 5)).toBe(BLACK);
    expect(fb.get(14, 12)).toBe(BLACK);
    expect(fb.get(6, 6)).toBe(WHITE);
    expect(fb.get(15, 5)).toBe(WHITE);
  });

  it("blits a sprite and honours the transparent index", () => {
    const fb = new FrameBuffer(WHITE);
    const sprite = {
      w: 2,
      h: 2,
      data: new Uint8Array([RED, WHITE, WHITE, YELLOW]),
    };
    fb.blitSprite(3, 4, sprite, WHITE);
    expect(fb.get(3, 4)).toBe(RED);
    expect(fb.get(4, 5)).toBe(YELLOW);
    expect(fb.get(4, 4)).toBe(WHITE);
  });

  it("scales a sprite by integer nearest-neighbour steps", () => {
    const fb = new FrameBuffer(WHITE);
    const sprite = { w: 1, h: 1, data: new Uint8Array([RED]) };
    fb.blitSprite(0, 0, sprite, undefined, 3);
    expect(fb.get(0, 0)).toBe(RED);
    expect(fb.get(2, 2)).toBe(RED);
    expect(fb.get(3, 3)).toBe(WHITE);
  });

  it("clones without aliasing the source", () => {
    const fb = new FrameBuffer(WHITE);
    const copy = fb.clone();
    copy.set(0, 0, RED);
    expect(fb.get(0, 0)).toBe(WHITE);
  });
});

describe("text rendering", () => {
  it("has a committed atlas for every family, weight and size on the ladder", () => {
    for (const family of FONT_FAMILY_IDS) {
      for (const weight of FONT_WEIGHTS) {
        for (const size of FONT_SIZES) {
          const atlas = font(family, weight, size);
          expect(atlas.size).toBe(size);
          expect(atlas.weight).toBe(weight);
          expect(atlas.family).toBe(family);
        }
      }
    }
    expect(availableAtlases()).toHaveLength(
      FONT_FAMILY_IDS.length * FONT_WEIGHTS.length * FONT_SIZES.length,
    );
  });

  it("refuses a size that has no atlas rather than guessing a near one", () => {
    expect(() => font("inter", "regular", 12)).toThrow(/No glyph atlas/);
  });

  it("draws ink only, leaving the rest of the frame untouched", () => {
    const fb = new FrameBuffer(WHITE);
    const width = fb.drawText(font("inter", "regular", 15), 12, 7, "KITCHEN", BLACK);
    expect(width).toBeGreaterThan(50);
    const inked = fb.pixels.reduce(
      (count, value) => (value === BLACK ? count + 1 : count),
      0,
    );
    expect(inked).toBeGreaterThan(80);
    // Nothing may land outside a generous box around the requested origin.
    for (let y = 0; y < FRAME_HEIGHT; y += 1) {
      for (let x = 0; x < FRAME_WIDTH; x += 1) {
        if (fb.get(x, y) !== BLACK) continue;
        expect(y).toBeGreaterThanOrEqual(7);
        expect(y).toBeLessThan(7 + 20);
        expect(x).toBeGreaterThanOrEqual(12);
      }
    }
  });

  it("is stable: the same string renders to the same bytes every time", () => {
    const render = (): string => {
      const fb = new FrameBuffer(WHITE);
      fb.drawText(font("inter", "bold", 22), 12, 7, "KITCHEN", BLACK);
      fb.drawText(font("inter", "regular", 13), 154, 13, "LES PROCHAINES 24H", BLACK);
      fb.drawText(font("atkinson", "regular", 18), 12, 65, "Meteo indisponible", RED);
      fb.drawText(font("plexmono", "regular", 11), 293, 280, "MAJ 11/09 01:07", BLACK);
      return sha256HexSync(pack(fb));
    };
    const first = render();
    expect(render()).toBe(first);
    expect(first).toBe(TEXT_SAMPLE_DIGEST);
  });

  it("truncates with an ellipsis to fit a width", () => {
    const atlas = font("inter", "regular", 15);
    const long = "A rather long meeting title for a narrow tile";
    const fitted = fitText(atlas, long, 120);
    expect(fitted.endsWith("…")).toBe(true);
    expect(measureText(atlas, fitted)).toBeLessThanOrEqual(120);
    expect(fitText(atlas, "court", 200)).toBe("court");
  });

  it("collapses whitespace the way the composer does", () => {
    const atlas = font("inter", "regular", 15);
    const fb = new FrameBuffer(WHITE);
    const a = fb.drawText(atlas, 0, 0, "  deux   mots \n", BLACK);
    const b = measureText(atlas, "deux mots");
    expect(a).toBeCloseTo(b, 6);
  });

  it("substitutes for codepoints outside the atlas rather than dropping them", () => {
    const atlas = font("inter", "regular", 15);
    expect(measureText(atlas, "中")).toBeGreaterThan(0);
    expect(measureText(atlas, "…")).toBeGreaterThan(0);
  });

  it("covers Latin-1 including the degree sign", () => {
    const atlas = font("atkinson", "regular", 18);
    for (const char of "0123456789°ÀÉèçùÎ") {
      expect(
        atlas.glyphs[String(char.codePointAt(0))],
        `missing glyph ${char}`,
      ).toBeDefined();
    }
  });

  it("aligns right and centre by measuring first", () => {
    const atlas = font("inter", "regular", 15);
    const width = measureText(atlas, "MAJ");
    const right = new FrameBuffer(WHITE);
    right.drawText(atlas, 100, 10, "MAJ", BLACK, { align: "right" });
    const left = new FrameBuffer(WHITE);
    left.drawText(atlas, 100 - width, 10, "MAJ", BLACK);
    expect(right.pixels).toEqual(left.pixels);
  });
});
