import type { FrameBuffer } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW, type PaletteIndex } from "@/core/palette";
import type { PixelRect } from "./types";
import {
  ditherRect,
  fillRectDither,
  scrubIsolatedAccents,
  type DitherStyle,
} from "./dither";
import { arc, polyline, thickLine } from "./draw";

/**
 * Chart & gauge components for the four-colour surface.
 *
 * These sit one level above {@link ./draw} primitives and {@link ./dither}
 * fills: they turn a value (or a series) into a bar, a ring, or a sparkline,
 * and they route every RED/YELLOW fill through the dither engine so the 2 px
 * accent law holds by construction. A module hands in the board's DitherStyle
 * (brush + texture from Expression) and gets a mark that matches its texture.
 *
 * PURITY: no clock, no Math.random. Same inputs → same indices → same bytes.
 */

const clamp01 = (v: number): number => (v < 0 ? 0 : v > 1 ? 1 : v);
const isAccent = (p: PaletteIndex): boolean => p === RED || p === YELLOW;

/* ------------------------------------------------------------------ *
 * Linear bar / gauge — a value as a filled track (progress, air quality)
 * ------------------------------------------------------------------ */

export interface BarGaugeOptions {
  /** 0..1 fraction of the track that is filled. */
  value: number;
  style: DitherStyle;
  /** Fill ink. Accent pigments are dithered; ink/paper are laid solid. */
  pigment?: PaletteIndex;
  background?: PaletteIndex;
  /** The surrounding rule colour. */
  frame?: PaletteIndex;
  /** Rule thickness in px (also the inset of the fill). Default 2. */
  frameWidth?: number;
}

/** A framed horizontal bar filled from the left to `value`. */
export function barGauge(
  fb: FrameBuffer,
  rect: PixelRect,
  opts: BarGaugeOptions,
): void {
  const frame = opts.frame ?? BLACK;
  const bg = opts.background ?? WHITE;
  const pigment = opts.pigment ?? YELLOW;
  const b = Math.max(1, Math.round(opts.frameWidth ?? 2));
  // A solid frame block hollowed out gives a clean b-px rule on every side.
  fb.fillRect(rect.x, rect.y, rect.w, rect.h, frame);
  const innerX = rect.x + b;
  const innerY = rect.y + b;
  const innerW = Math.max(0, rect.w - 2 * b);
  const innerH = Math.max(0, rect.h - 2 * b);
  if (innerW <= 0 || innerH <= 0) return;
  fb.fillRect(innerX, innerY, innerW, innerH, bg);
  const fillW = Math.round(innerW * clamp01(opts.value));
  if (fillW <= 0) return;
  const cell: PixelRect = { x: innerX, y: innerY, w: fillW, h: innerH };
  if (isAccent(pigment)) {
    fillRectDither(fb, cell, pigment, 1, opts.style, bg);
  } else {
    fb.fillRect(innerX, innerY, fillW, innerH, pigment);
  }
}

/* ------------------------------------------------------------------ *
 * Bar series — a row of vertical bars (weekly steps, price chart)
 * ------------------------------------------------------------------ */

export interface BarSpec {
  pigment: PaletteIndex;
  /** false draws an outline-only bar (used for "normal" values). Default true. */
  filled?: boolean;
}

export interface BarSeriesOptions {
  /** Each value 0..1, already normalised to the rect height. */
  values: readonly number[];
  style: DitherStyle;
  /** Pixels between bars. Default 2. */
  gap?: number;
  background?: PaletteIndex;
  /** Decide each bar's ink; default solid black. */
  colorFor?: (index: number, value: number) => BarSpec;
}

/** A row of vertical bars growing up from the bottom of `rect`. */
export function barSeries(
  fb: FrameBuffer,
  rect: PixelRect,
  opts: BarSeriesOptions,
): void {
  const n = opts.values.length;
  if (n === 0) return;
  const gap = Math.max(0, opts.gap ?? 2);
  const bg = opts.background ?? WHITE;
  const bw = Math.max(1, Math.floor((rect.w - gap * (n - 1)) / n));
  for (let i = 0; i < n; i += 1) {
    const v = clamp01(opts.values[i] as number);
    const bh = Math.round(rect.h * v);
    if (bh <= 0) continue;
    const bx = rect.x + i * (bw + gap);
    const by = rect.y + rect.h - bh;
    const spec: BarSpec = opts.colorFor
      ? opts.colorFor(i, v)
      : { pigment: BLACK };
    const cell: PixelRect = { x: bx, y: by, w: bw, h: bh };
    if (spec.filled === false) {
      fb.strokeRect(bx, by, bw, bh, spec.pigment);
    } else if (isAccent(spec.pigment)) {
      fillRectDither(fb, cell, spec.pigment, 1, opts.style, bg);
    } else {
      fb.fillRect(bx, by, bw, bh, spec.pigment);
    }
  }
}

/* ------------------------------------------------------------------ *
 * Sparkline — a series as a trend line with an optional toned area
 * ------------------------------------------------------------------ */

export interface SparklineOptions {
  values: readonly number[];
  /** Line ink. Default black. */
  color?: PaletteIndex;
  /** Line width in px. Default 2. */
  width?: number;
  /** Data range; defaults to the series' own min/max. */
  min?: number;
  max?: number;
  /** 0..1 coverage of a dithered fill under the line. 0/undefined = none. */
  area?: number;
  areaPigment?: PaletteIndex;
  style?: DitherStyle;
  background?: PaletteIndex;
}

/** A trend line fitted to `rect`, optionally over a dithered area fill. */
export function sparkline(
  fb: FrameBuffer,
  rect: PixelRect,
  opts: SparklineOptions,
): void {
  const vals = opts.values;
  const n = vals.length;
  if (n < 2) return;
  const color = opts.color ?? BLACK;
  const lw = Math.max(1, Math.round(opts.width ?? 2));
  const bg = opts.background ?? WHITE;
  const lo = opts.min ?? Math.min(...vals);
  const hi = opts.max ?? Math.max(...vals);
  const span = hi - lo || 1;
  const xAt = (i: number): number => rect.x + ((rect.w - 1) * i) / (n - 1);
  const yAt = (value: number): number =>
    rect.y + (rect.h - 1) * (1 - clamp01((value - lo) / span));
  const yLineAt = (x: number): number => {
    const fi = clamp01((x - rect.x) / Math.max(1, rect.w - 1)) * (n - 1);
    const i = Math.min(n - 2, Math.floor(fi));
    const t = fi - i;
    const ya = yAt(vals[i] as number);
    const yb = yAt(vals[i + 1] as number);
    return ya + (yb - ya) * t;
  };
  const areaPigment = opts.areaPigment ?? color;
  if (opts.area && opts.area > 0 && opts.style) {
    ditherRect(fb, rect, {
      pigment: areaPigment,
      background: bg,
      brush: opts.style.brush,
      texture: opts.style.texture,
      tone: clamp01(opts.area),
      inside: (x, y) => y >= yLineAt(x),
    });
  }
  const pts: number[] = [];
  for (let i = 0; i < n; i += 1) {
    pts.push(xAt(i), yAt(vals[i] as number));
  }
  if (lw <= 1) {
    polyline(fb, pts, color);
  } else {
    for (let i = 0; i + 3 < pts.length; i += 2) {
      thickLine(
        fb,
        pts[i] as number,
        pts[i + 1] as number,
        pts[i + 2] as number,
        pts[i + 3] as number,
        color,
        lw,
      );
    }
  }
  // Drawing an ink line over an accent area can clip a block to a sliver.
  if (opts.area && opts.area > 0 && isAccent(areaPigment)) {
    scrubIsolatedAccents(fb, rect, bg);
  }
}

/* ------------------------------------------------------------------ *
 * Ring gauge — a value as a filled annular arc (activity rings)
 * ------------------------------------------------------------------ */

export interface RingGaugeOptions {
  /** 0..1 of a full turn. */
  value: number;
  style: DitherStyle;
  /** Ring thickness in px (>= 2). */
  thickness: number;
  pigment?: PaletteIndex;
  /** Where the arc begins, degrees clockwise from the top. Default 0. */
  startDeg?: number;
  /** Optional thin ring outline drawn as a track. */
  track?: PaletteIndex;
  background?: PaletteIndex;
}

/** A dithered annular arc sweeping `value` of a full turn from `startDeg`. */
export function ringGauge(
  fb: FrameBuffer,
  cx: number,
  cy: number,
  rOuter: number,
  opts: RingGaugeOptions,
): void {
  if (rOuter <= 0) return;
  const th = Math.max(2, Math.round(opts.thickness));
  const rInner = rOuter - th;
  if (rInner <= 0) return;
  const pigment = opts.pigment ?? RED;
  const bg = opts.background ?? WHITE;
  const start = opts.startDeg ?? 0;
  const sweep = 360 * clamp01(opts.value);
  const rect: PixelRect = {
    x: Math.floor(cx - rOuter),
    y: Math.floor(cy - rOuter),
    w: Math.ceil(rOuter * 2) + 1,
    h: Math.ceil(rOuter * 2) + 1,
  };
  if (sweep > 0) {
    ditherRect(fb, rect, {
      pigment,
      background: bg,
      brush: opts.style.brush,
      texture: opts.style.texture,
      tone: 1,
      inside: (x, y) => {
        const dx = x - cx;
        const dy = y - cy;
        const rr = Math.sqrt(dx * dx + dy * dy);
        if (rr < rInner || rr > rOuter) return false;
        let ang = (Math.atan2(dx, -dy) * 180) / Math.PI;
        if (ang < 0) ang += 360;
        const rel = (((ang - start) % 360) + 360) % 360;
        return rel <= sweep;
      },
    });
  }
  if (opts.track !== undefined) {
    arc(fb, cx, cy, rOuter, 0, 360, opts.track, 1);
    arc(fb, cx, cy, rInner, 0, 360, opts.track, 1);
    if (isAccent(pigment)) scrubIsolatedAccents(fb, rect, bg);
  }
}
