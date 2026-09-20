import { FrameBuffer } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW, type PaletteIndex } from "@/core/palette";
import {
  MIN_ACCENT_CELL,
  type Brush,
  type ColourUse,
  type PixelTexture,
} from "@/core/theme";
import type { PixelRect } from "./types";

/**
 * The dither engine: BWRY ordered dithering for an editorial / risograph
 * surface.
 *
 * WHY THIS EXISTS
 * ---------------
 * The panel prints four inks and nothing between them. A flat field of solid
 * red is loud and a flat field of solid black is a hole; a *toned* field — the
 * ink laid down through an ordered pattern so it reads as 40% or 70% coverage —
 * is what gives a four-colour panel the warmth of a printed poster. This module
 * turns a pigment plus a coverage plus a brush into palette indices, and it is
 * the single place that knows how.
 *
 * THE ONE PHYSICAL LAW
 * --------------------
 * RED and YELLOW never render finer than a 2 px cell. A single isolated warm
 * pixel does not develop on this panel, so a 1 px red speck is worse than no
 * red at all: it is a dropout that reads as damage. This is enforced two ways,
 * on purpose, so no future brush can defeat it:
 *
 *   1. STRUCTURALLY, up front: `cellFor()` clamps the cell size of an accent
 *      pigment to at least {@link MIN_ACCENT_CELL}, so every accent mark this
 *      engine lays is a solid block of at least 2x2.
 *   2. DEFENSIVELY, after the fact: `scrubIsolatedAccents()` removes any accent
 *      pixel left with no same-pigment neighbour — the sliver a sharp mask edge
 *      can still cut from an otherwise-legal block. Black and white are never
 *      touched; they are allowed single pixels, and the panel prints them.
 *
 * PURITY
 * ------
 * No Node APIs, no Math.random, no clock. Every mark is a function of its
 * position through an ordered matrix, so the browser preview and the packed
 * device bytes are identical to the pixel. The matrices are anchored to global
 * frame coordinates rather than to a rect's own origin, so two fields that meet
 * share one continuous texture instead of showing a seam.
 */

/* ------------------------------------------------------------------ *
 * Ordered matrices
 * ------------------------------------------------------------------ */

/**
 * Bayer 8x8, dispersed. The classic even ordered-dither threshold map: values
 * spread as far from their neighbours as possible, so a low coverage reads as a
 * fine, uniform speckle rather than as clumps. This is the Grain brush.
 */
const BAYER_8: readonly number[] = [
  0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36,
  14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22, 3, 35, 11, 43, 1, 33, 9, 41,
  51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23,
  61, 29, 53, 21,
];

/**
 * Clustered-dot 8x8. Thresholds spiral outward from two dot centres, so as a
 * tone deepens the inked cells grow from those centres into fat dots — the look
 * of a halftoned newspaper photograph. This is the Halftone brush.
 */
const CLUSTER_8: readonly number[] = [
  24, 10, 12, 26, 35, 47, 49, 37, 8, 0, 2, 14, 45, 59, 61, 51, 22, 6, 4, 16, 43,
  57, 63, 53, 30, 20, 18, 28, 33, 41, 55, 39, 34, 46, 48, 36, 25, 11, 13, 27,
  44, 58, 60, 50, 9, 1, 3, 15, 42, 56, 62, 52, 23, 7, 5, 17, 32, 40, 54, 38, 31,
  21, 19, 29,
];

const MATRIX_SIDE = 8;

/** Threshold in [0,1) for a brush that is driven by a coverage matrix. */
function matrixThreshold(matrix: readonly number[], cellX: number, cellY: number): number {
  const x = ((cellX % MATRIX_SIDE) + MATRIX_SIDE) % MATRIX_SIDE;
  const y = ((cellY % MATRIX_SIDE) + MATRIX_SIDE) % MATRIX_SIDE;
  const value = matrix[y * MATRIX_SIDE + x] ?? 0;
  return (value + 0.5) / (MATRIX_SIDE * MATRIX_SIDE);
}

/**
 * The Grid brush: an ordered crosshatch that reads as letterpress hatching.
 *
 * It is not a coverage matrix. It draws two families of diagonal lines on the
 * cell grid, which alone cover about 0.44 of the field, and it lets a deepening
 * tone add the denser in-between diagonals rather than smoothly fading dots.
 * Below a floor it draws nothing, so the palest end of a gradient stays paper.
 */
function gridInk(cellX: number, cellY: number, tone: number): boolean {
  if (tone <= 0.08) return false;
  const sum = ((cellX + cellY) % 4 + 4) % 4;
  const diff = ((cellX - cellY) % 4 + 4) % 4;
  let ink = sum === 0 || diff === 0; // ~0.44 coverage
  if (tone > 0.6) ink = ink || (((cellX + cellY) % 2 + 2) % 2) === 0;
  if (tone > 0.85) ink = ink || (((cellX - cellY) % 2 + 2) % 2) === 0;
  return ink;
}

/** Does this brush ink the cell at (cellX, cellY) for the given tone? */
export function brushInk(
  brush: Brush,
  cellX: number,
  cellY: number,
  tone: number,
): boolean {
  const t = clamp01(tone);
  if (t <= 0) return false;
  if (t >= 1) return true;
  switch (brush) {
    case "halftone":
      return t > matrixThreshold(CLUSTER_8, cellX, cellY);
    case "grid":
      return gridInk(cellX, cellY, t);
    case "grain":
    default:
      return t > matrixThreshold(BAYER_8, cellX, cellY);
  }
}

/* ------------------------------------------------------------------ *
 * Cell sizing — where the 2 px accent law is enforced structurally
 * ------------------------------------------------------------------ */

const BASE_CELL: Record<PixelTexture, number> = {
  fine: 1,
  medium: 2,
  large: 3,
};

/**
 * The side, in pixels, of one atomic cell for this pigment at this texture.
 *
 * Black and white may go down to a single pixel at the finest texture, because
 * the panel prints a lone black or white pixel. Red and yellow are clamped up
 * to {@link MIN_ACCENT_CELL}: this is the line that makes an isolated warm
 * pixel unrepresentable rather than merely discouraged. Change BASE_CELL all
 * you like; an accent can never come out below 2.
 */
export function cellFor(pigment: PaletteIndex, texture: PixelTexture): number {
  const base = BASE_CELL[texture] ?? 2;
  const min = pigment === RED || pigment === YELLOW ? MIN_ACCENT_CELL : 1;
  return Math.max(min, base);
}

/* ------------------------------------------------------------------ *
 * Fills
 * ------------------------------------------------------------------ */

export interface DitherStyle {
  brush: Brush;
  texture: PixelTexture;
}

export interface DitherFill {
  /** The ink the marks are made in. */
  pigment: PaletteIndex;
  /** The colour a cell shows when the brush does not ink it. Usually paper. */
  background?: PaletteIndex;
  /**
   * Coverage in [0,1]. A number for a flat toned field, or a function of the
   * pixel position for a gradient or a shaded shape.
   */
  tone: number | ((x: number, y: number) => number);
  /**
   * Optional shape mask, evaluated at each cell's centre. A cell is considered
   * part of the shape when its centre is inside; the whole cell is then filled,
   * so an accent edge is quantised to the cell grid and never split into a
   * sliver. Omit to fill the whole rect.
   */
  inside?: (x: number, y: number) => boolean;
  brush: Brush;
  texture: PixelTexture;
}

function clamp01(value: number): number {
  if (value < 0) return 0;
  if (value > 1) return 1;
  return value;
}

/**
 * Fill a rectangle (optionally masked to a shape) with a dithered pigment.
 *
 * This is the primitive every other fill in this module is built on. It walks
 * the rect one cell at a time, anchored to global frame coordinates so the
 * texture is continuous across neighbouring fills, decides ink from the brush,
 * and paints whole cells. Accent slivers are scrubbed at the end.
 */
export function ditherRect(fb: FrameBuffer, rect: PixelRect, fill: DitherFill): void {
  const x0 = Math.max(0, Math.trunc(rect.x));
  const y0 = Math.max(0, Math.trunc(rect.y));
  const x1 = Math.min(fb.width, Math.trunc(rect.x + rect.w));
  const y1 = Math.min(fb.height, Math.trunc(rect.y + rect.h));
  if (x1 <= x0 || y1 <= y0) return;

  const cell = cellFor(fill.pigment, fill.texture);
  const background = fill.background ?? WHITE;
  const toneAt =
    typeof fill.tone === "function" ? fill.tone : () => fill.tone as number;

  // Snap the cell lattice to global coordinates so adjacent fills share it.
  const startCellX = Math.floor(x0 / cell);
  const startCellY = Math.floor(y0 / cell);
  const endCellX = Math.floor((x1 - 1) / cell);
  const endCellY = Math.floor((y1 - 1) / cell);

  for (let cy = startCellY; cy <= endCellY; cy += 1) {
    const cellTop = cy * cell;
    const py0 = Math.max(y0, cellTop);
    const py1 = Math.min(y1, cellTop + cell);
    const centreY = cellTop + (cell - 1) / 2;
    for (let cx = startCellX; cx <= endCellX; cx += 1) {
      const cellLeft = cx * cell;
      const centreX = cellLeft + (cell - 1) / 2;
      if (fill.inside && !fill.inside(centreX, centreY)) continue;
      const tone = clamp01(toneAt(centreX, centreY));
      const colour: PaletteIndex = brushInk(fill.brush, cx, cy, tone)
        ? fill.pigment
        : background;
      // Background WHITE on an already-white frame is a no-op we still write,
      // so a masked shape clears its own interior consistently.
      const px0 = Math.max(x0, cellLeft);
      const px1 = Math.min(x1, cellLeft + cell);
      fb.fillRect(px0, py0, px1 - px0, py1 - py0, colour);
    }
  }

  if (fill.pigment === RED || fill.pigment === YELLOW) {
    scrubIsolatedAccents(fb, { x: x0, y: y0, w: x1 - x0, h: y1 - y0 }, background);
  }
}

/** A flat toned rectangle. The simplest surface: one pigment, one coverage. */
export function fillRectDither(
  fb: FrameBuffer,
  rect: PixelRect,
  pigment: PaletteIndex,
  coverage: number,
  style: DitherStyle,
  background: PaletteIndex = WHITE,
): void {
  ditherRect(fb, rect, {
    pigment,
    background,
    tone: coverage,
    brush: style.brush,
    texture: style.texture,
  });
}

/**
 * A horizontal tonal band whose coverage ramps from one edge to the other.
 *
 * The workhorse of the editorial look: a colour bar that is dense at one side
 * and fades to paper at the other, so a headline set over it stays legible
 * where the type lands and the field still carries weight where it does not.
 */
export function fillBandDither(
  fb: FrameBuffer,
  rect: PixelRect,
  pigment: PaletteIndex,
  opts: {
    from: number;
    to: number;
    axis?: "x" | "y";
    style: DitherStyle;
    background?: PaletteIndex;
  },
): void {
  const axis = opts.axis ?? "y";
  const span = axis === "y" ? Math.max(1, rect.h) : Math.max(1, rect.w);
  const origin = axis === "y" ? rect.y : rect.x;
  const tone = (x: number, y: number): number => {
    const along = (axis === "y" ? y : x) - origin;
    const t = clamp01(along / span);
    return opts.from + (opts.to - opts.from) * t;
  };
  ditherRect(fb, rect, {
    pigment,
    ...(opts.background !== undefined ? { background: opts.background } : {}),
    tone,
    brush: opts.style.brush,
    texture: opts.style.texture,
  });
}

/**
 * A filled disc — a sun or a moon — laid down as a dithered shape.
 *
 * `edgeSoftness` shades the disc so its rim is lighter than its core, which on
 * a warm pigment reads as a glowing body rather than a flat coin. Zero gives a
 * uniform coverage.
 */
export function fillDiscDither(
  fb: FrameBuffer,
  cx: number,
  cy: number,
  radius: number,
  pigment: PaletteIndex,
  coverage: number,
  style: DitherStyle,
  opts: { background?: PaletteIndex; edgeSoftness?: number } = {},
): void {
  if (radius <= 0) return;
  const r2 = radius * radius;
  const soft = clamp01(opts.edgeSoftness ?? 0);
  const rect: PixelRect = {
    x: Math.floor(cx - radius),
    y: Math.floor(cy - radius),
    w: Math.ceil(radius * 2) + 1,
    h: Math.ceil(radius * 2) + 1,
  };
  ditherRect(fb, rect, {
    pigment,
    ...(opts.background !== undefined ? { background: opts.background } : {}),
    inside: (x, y) => (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r2,
    tone: (x, y) => {
      if (soft <= 0) return coverage;
      const d = Math.sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius;
      // Full coverage in the core, dropping to (1 - soft) of it at the rim.
      return coverage * (1 - soft * clamp01(d));
    },
    brush: style.brush,
    texture: style.texture,
  });
}

/* ------------------------------------------------------------------ *
 * The accent-sliver scrub
 * ------------------------------------------------------------------ */

/**
 * Remove any RED or YELLOW pixel that has no orthogonally-or-diagonally
 * adjacent pixel of the same pigment, replacing it with `background`.
 *
 * This is the second half of the 2 px guarantee. Cell-fills already keep every
 * accent mark a solid block, but a shape mask can clip a block down to a lone
 * corner pixel; this pass catches exactly those. It never touches black or
 * white, which the panel prints at a single pixel.
 */
export function scrubIsolatedAccents(
  fb: FrameBuffer,
  rect: PixelRect,
  background: PaletteIndex = WHITE,
): void {
  const x0 = Math.max(0, Math.trunc(rect.x));
  const y0 = Math.max(0, Math.trunc(rect.y));
  const x1 = Math.min(fb.width, Math.trunc(rect.x + rect.w));
  const y1 = Math.min(fb.height, Math.trunc(rect.y + rect.h));
  const doomed: number[] = [];
  for (let y = y0; y < y1; y += 1) {
    for (let x = x0; x < x1; x += 1) {
      const here = fb.get(x, y);
      if (here !== RED && here !== YELLOW) continue;
      let hasNeighbour = false;
      for (let dy = -1; dy <= 1 && !hasNeighbour; dy += 1) {
        for (let dx = -1; dx <= 1; dx += 1) {
          if (dx === 0 && dy === 0) continue;
          if (fb.get(x + dx, y + dy) === here) {
            hasNeighbour = true;
            break;
          }
        }
      }
      if (!hasNeighbour) doomed.push(y * fb.width + x);
    }
  }
  for (const offset of doomed) fb.pixels[offset] = background;
}

/**
 * Scan a region and return the count of accent pixels that are isolated, i.e.
 * that break the 2 px rule. Zero is the only acceptable answer for a shipped
 * frame; the tests assert it, and the scrub above guarantees it.
 */
export function countIsolatedAccents(fb: FrameBuffer, rect?: PixelRect): number {
  const x0 = rect ? Math.max(0, Math.trunc(rect.x)) : 0;
  const y0 = rect ? Math.max(0, Math.trunc(rect.y)) : 0;
  const x1 = rect ? Math.min(fb.width, Math.trunc(rect.x + rect.w)) : fb.width;
  const y1 = rect ? Math.min(fb.height, Math.trunc(rect.y + rect.h)) : fb.height;
  let count = 0;
  for (let y = y0; y < y1; y += 1) {
    for (let x = x0; x < x1; x += 1) {
      const here = fb.get(x, y);
      if (here !== RED && here !== YELLOW) continue;
      let hasNeighbour = false;
      for (let dy = -1; dy <= 1 && !hasNeighbour; dy += 1) {
        for (let dx = -1; dx <= 1; dx += 1) {
          if (dx === 0 && dy === 0) continue;
          if (fb.get(x + dx, y + dy) === here) {
            hasNeighbour = true;
            break;
          }
        }
      }
      if (!hasNeighbour) count += 1;
    }
  }
  return count;
}

/* ------------------------------------------------------------------ *
 * Expression → pigment budget
 * ------------------------------------------------------------------ */

/**
 * How much accent ink a colour-use stance is willing to spend, as a coverage
 * multiplier, and whether it spends any at all.
 *
 * "Black & white" returns zero and reports it, so a module can fall back to an
 * ink tone instead of drawing an invisible warm field. "Balanced" is restraint;
 * "Expressive" gives the pigment room without ever reaching a flat solid — the
 * ceiling stays below 1 so even the boldest field keeps its printed tooth.
 */
export interface AccentBudget {
  /** Whether warm pigment may be spent at all. */
  usesAccent: boolean;
  /** Multiplier applied to a module's requested accent coverage. */
  scale: number;
  /** A sensible ceiling for a "full" accent field under this stance. */
  ceiling: number;
}

export function accentBudget(colourUse: ColourUse): AccentBudget {
  switch (colourUse) {
    case "blackwhite":
      return { usesAccent: false, scale: 0, ceiling: 0 };
    case "expressive":
      return { usesAccent: true, scale: 1, ceiling: 0.86 };
    case "balanced":
    default:
      return { usesAccent: true, scale: 0.62, ceiling: 0.6 };
  }
}

/** The neutral ink a black & white stance uses where a warm field would go. */
export const INK_FALLBACK: PaletteIndex = BLACK;

export { BLACK, RED, WHITE, YELLOW };
