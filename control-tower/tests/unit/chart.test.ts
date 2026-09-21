import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW } from "@/core/palette";
import { countIsolatedAccents, type DitherStyle } from "@/core/render/dither";
import { arc, thickLine } from "@/core/render/draw";
import { barGauge, barSeries, ringGauge, sparkline } from "@/core/render/chart";
import { sha256HexSync } from "@/server/hash";

const STYLE: DitherStyle = { brush: "grain", texture: "medium" };
const fresh = (): FrameBuffer => new FrameBuffer(WHITE);
const digest = (fb: FrameBuffer): string => sha256HexSync(pack(fb));
const count = (fb: FrameBuffer, idx: number): number => {
  let c = 0;
  for (const p of fb.pixels) if (p === idx) c += 1;
  return c;
};
const anyIn = (
  fb: FrameBuffer,
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  idx: number,
): boolean => {
  for (let y = y0; y < y1; y += 1)
    for (let x = x0; x < x1; x += 1) if (fb.get(x, y) === idx) return true;
  return false;
};

describe("barGauge", () => {
  it("fills a dithered accent track, no isolated accent, deterministic", () => {
    const a = fresh();
    barGauge(a, { x: 10, y: 10, w: 200, h: 20 }, { value: 0.5, style: STYLE, pigment: YELLOW });
    expect(() => pack(a)).not.toThrow();
    expect(countIsolatedAccents(a)).toBe(0);
    expect(count(a, YELLOW)).toBeGreaterThan(0);
    // fill is on the left half only
    expect(anyIn(a, 12, 12, 100, 28, YELLOW)).toBe(true);
    expect(anyIn(a, 150, 12, 208, 28, YELLOW)).toBe(false);
    const b = fresh();
    barGauge(b, { x: 10, y: 10, w: 200, h: 20 }, { value: 0.5, style: STYLE, pigment: YELLOW });
    expect(digest(a)).toBe(digest(b));
  });

  it("value 0 paints no accent at all", () => {
    const a = fresh();
    barGauge(a, { x: 10, y: 10, w: 200, h: 20 }, { value: 0, style: STYLE, pigment: RED });
    expect(count(a, RED)).toBe(0);
  });
});

describe("barSeries", () => {
  it("mixes solid, dithered accent and outline bars while staying accent-safe", () => {
    const fb = fresh();
    barSeries(
      fb,
      { x: 0, y: 0, w: 300, h: 100 },
      {
        values: [0.2, 0.6, 0.4, 0.9, 0.5, 0.7, 0.55],
        style: STYLE,
        colorFor: (_i, v) =>
          v >= 0.8 ? { pigment: RED } : v <= 0.3 ? { pigment: YELLOW } : { pigment: BLACK, filled: false },
      },
    );
    expect(() => pack(fb)).not.toThrow();
    expect(countIsolatedAccents(fb)).toBe(0);
    expect(count(fb, RED)).toBeGreaterThan(0);
    expect(count(fb, YELLOW)).toBeGreaterThan(0);
  });
});

describe("sparkline", () => {
  it("draws a dithered area under an ink line and stays accent-safe", () => {
    const fb = fresh();
    sparkline(
      fb,
      { x: 0, y: 0, w: 200, h: 60 },
      { values: [3, 5, 4, 8, 6, 9, 7, 11], color: BLACK, width: 2, area: 0.5, areaPigment: YELLOW, style: STYLE },
    );
    expect(() => pack(fb)).not.toThrow();
    expect(countIsolatedAccents(fb)).toBe(0);
    expect(count(fb, BLACK)).toBeGreaterThan(0);
  });

  it("plots a rising series climbing left-to-right", () => {
    const fb = fresh();
    sparkline(fb, { x: 0, y: 0, w: 100, h: 50 }, { values: [0, 1, 2, 3], color: BLACK, width: 1 });
    expect(anyIn(fb, 0, 25, 50, 50, BLACK)).toBe(true); // low at the left
    expect(anyIn(fb, 50, 0, 100, 25, BLACK)).toBe(true); // high at the right
  });
});

describe("ringGauge", () => {
  it("sweeps a partial dithered arc, accent-safe and deterministic", () => {
    const a = fresh();
    ringGauge(a, 100, 100, 40, { value: 0.7, thickness: 9, pigment: RED, style: STYLE });
    expect(() => pack(a)).not.toThrow();
    expect(countIsolatedAccents(a)).toBe(0);
    expect(count(a, RED)).toBeGreaterThan(0);
    const b = fresh();
    ringGauge(b, 100, 100, 40, { value: 0.7, thickness: 9, pigment: RED, style: STYLE });
    expect(digest(a)).toBe(digest(b));
  });

  it("keeps accents clean even when an ink track is drawn over the arc", () => {
    const fb = fresh();
    ringGauge(fb, 100, 100, 40, { value: 0.4, thickness: 9, pigment: RED, style: STYLE, track: BLACK });
    expect(countIsolatedAccents(fb)).toBe(0);
  });
});

describe("draw primitives", () => {
  it("thickLine and arc write only palette indices and clip out of bounds", () => {
    const fb = fresh();
    thickLine(fb, -5, -5, 420, 320, BLACK, 3);
    arc(fb, 200, 150, 60, 0, 360, BLACK, 2);
    expect(() => pack(fb)).not.toThrow();
    expect(count(fb, BLACK)).toBeGreaterThan(0);
  });
});
