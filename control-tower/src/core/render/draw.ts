import type { FrameBuffer } from "@/core/frame";
import type { PaletteIndex } from "@/core/palette";

/**
 * Pillow-compatible primitives.
 *
 * The composer's sprite art is written against PIL's ImageDraw, whose
 * rectangle and ellipse take INCLUSIVE corner coordinates and whose line is
 * an inclusive Bresenham run. Porting those coordinates verbatim only works
 * if these helpers share that convention, so they do.
 */

export interface Point {
  x: number;
  y: number;
}

/** PIL ImageDraw.rectangle: both corners included. */
export function rectInclusive(
  fb: FrameBuffer,
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  color: PaletteIndex,
): void {
  const left = Math.min(x0, x1);
  const right = Math.max(x0, x1);
  const top = Math.min(y0, y1);
  const bottom = Math.max(y0, y1);
  fb.fillRect(left, top, right - left + 1, bottom - top + 1, color);
}

/** PIL ImageDraw.line for a single segment, width 1. */
export function line(
  fb: FrameBuffer,
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  color: PaletteIndex,
): void {
  let x = Math.round(x0);
  let y = Math.round(y0);
  const ex = Math.round(x1);
  const ey = Math.round(y1);
  const dx = Math.abs(ex - x);
  const dy = -Math.abs(ey - y);
  const sx = x < ex ? 1 : -1;
  const sy = y < ey ? 1 : -1;
  let err = dx + dy;

  for (;;) {
    fb.set(x, y, color);
    if (x === ex && y === ey) break;
    const e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y += sy;
    }
  }
}

/** PIL passes a flat coordinate list to line() and joins the points. */
export function polyline(
  fb: FrameBuffer,
  coords: readonly number[],
  color: PaletteIndex,
): void {
  for (let i = 0; i + 3 < coords.length; i += 2) {
    line(
      fb,
      coords[i] as number,
      coords[i + 1] as number,
      coords[i + 2] as number,
      coords[i + 3] as number,
      color,
    );
  }
}

/** Filled polygon, scanline, with the outline included as PIL does. */
export function polygon(
  fb: FrameBuffer,
  points: readonly Point[],
  color: PaletteIndex,
): void {
  if (points.length < 3) return;
  const ys = points.map((p) => p.y);
  const top = Math.ceil(Math.min(...ys));
  const bottom = Math.floor(Math.max(...ys));

  for (let y = top; y <= bottom; y += 1) {
    const crossings: number[] = [];
    for (let i = 0; i < points.length; i += 1) {
      const a = points[i] as Point;
      const b = points[(i + 1) % points.length] as Point;
      if (a.y === b.y) continue;
      const lower = Math.min(a.y, b.y);
      const upper = Math.max(a.y, b.y);
      if (y < lower || y >= upper) continue;
      crossings.push(a.x + ((y - a.y) / (b.y - a.y)) * (b.x - a.x));
    }
    crossings.sort((p, q) => p - q);
    for (let i = 0; i + 1 < crossings.length; i += 2) {
      const from = Math.ceil(crossings[i] as number);
      const to = Math.floor(crossings[i + 1] as number);
      if (to >= from) fb.fillRect(from, y, to - from + 1, 1, color);
    }
  }

  for (let i = 0; i < points.length; i += 1) {
    const a = points[i] as Point;
    const b = points[(i + 1) % points.length] as Point;
    line(fb, a.x, a.y, b.x, b.y, color);
  }
}

/** PIL ImageDraw.ellipse: inclusive bounding box, optional fill and outline. */
export function ellipseInclusive(
  fb: FrameBuffer,
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  fill?: PaletteIndex,
  outline?: PaletteIndex,
): void {
  const left = Math.min(x0, x1);
  const right = Math.max(x0, x1);
  const top = Math.min(y0, y1);
  const bottom = Math.max(y0, y1);
  // Analytic ellipse: radii are the half-extents plus half a pixel, and each
  // row keeps the integer columns inside the curve.
  //
  // Pillow fills an ellipse by scan-filling a polygon of sampled boundary
  // points, so its silhouette is a coarse approximation that this rule matches
  // on many boxes but not all. The difference is at most a pixel or two of
  // outline on the ported weather icon, it never changes a colour index, and
  // pack() parity with the composer is unaffected. Text and the octopus sprite
  // are pixel-identical to Pillow; see tools/parity_check.py.
  const cx = (left + right) / 2;
  const cy = (top + bottom) / 2;
  const rx = (right - left) / 2 + 0.5;
  const ry = (bottom - top) / 2 + 0.5;
  if (rx <= 0 || ry <= 0) return;

  const spans: Array<[number, number] | null> = [];
  for (let y = top; y <= bottom; y += 1) {
    const dy = (y - cy) / ry;
    const inside = 1 - dy * dy;
    if (inside < 0) {
      spans.push(null);
      continue;
    }
    const dx = rx * Math.sqrt(inside);
    const from = Math.ceil(cx - dx);
    const to = Math.floor(cx + dx);
    if (to < from) {
      spans.push(null);
      continue;
    }
    spans.push([from, to]);
    if (fill !== undefined) fb.fillRect(from, y, to - from + 1, 1, fill);
  }

  if (outline === undefined) return;

  // Pillow strokes the 4-connected border of the filled region: a filled pixel
  // is outline when any of its four neighbours falls outside the ellipse.
  const covered = (index: number, x: number): boolean => {
    const span = spans[index];
    return span !== undefined && span !== null && x >= span[0] && x <= span[1];
  };

  for (let i = 0; i < spans.length; i += 1) {
    const span = spans[i];
    if (!span) continue;
    const y = top + i;
    const [from, to] = span;
    for (let x = from; x <= to; x += 1) {
      if (
        x === from ||
        x === to ||
        !covered(i - 1, x) ||
        !covered(i + 1, x)
      ) {
        fb.set(x, y, outline);
      }
    }
  }
}
