import type { FrameBuffer } from "@/core/frame";
import { BLACK, type PaletteIndex } from "@/core/palette";
import type { PixelRect } from "./types";
import { dashedLine } from "./draw";

/**
 * A module's frame: the hairline rules that delimit it on the panel.
 *
 * This is the structural device the compositions were missing. Modules tile
 * the grid edge to edge with no gutter, so without a rule a weather block and
 * an agenda block simply abut and read as one undifferentiated field. A single
 * black hairline on the shared edge is what turns two adjacent modules into two
 * legible regions — the same move a newspaper makes with a column rule.
 *
 * Rules are drawn AFTER the module paints, so they always sit on top of its own
 * ink at the very edge of its rectangle, and they grow INWARD by `weight` so a
 * rule never spills into a neighbour. A vertical rule on a module's right edge
 * and its neighbour's left edge would both land on the same seam; author one,
 * not both.
 *
 * Black by design: the rule is chrome, not data, and black is the one pigment
 * the palette policy never disables, so a frame survives a black-and-white
 * theme unchanged.
 */
export type FrameEdge = "top" | "right" | "bottom" | "left";

export interface FrameSpec {
  edges: readonly FrameEdge[];
  /** Rule thickness in pixels, grown inward from the edge. */
  weight?: number;
  style?: "solid" | "dashed" | "dotted";
  /** Fraction (0..0.45) trimmed off each END of the rule, for an inset hairline. */
  inset?: number;
  /** Palette index (0..3). The model validates the range; we narrow here. */
  color?: number;
}

function dashFor(style: "dashed" | "dotted"): { dash: number; gap: number } {
  return style === "dotted" ? { dash: 1, gap: 2 } : { dash: 3, gap: 3 };
}

/** Draw the requested edges of `rect` as hairline rules. */
export function drawModuleFrame(
  fb: FrameBuffer,
  rect: PixelRect,
  frame: FrameSpec,
): void {
  if (!frame.edges || frame.edges.length === 0) return;

  const weight = Math.max(1, Math.min(4, Math.round(frame.weight ?? 2)));
  const color = ((frame.color ?? BLACK) & 3) as PaletteIndex;
  const style = frame.style ?? "solid";
  const inset = Math.max(0, Math.min(0.45, frame.inset ?? 0));
  const { x, y, w, h } = rect;
  if (w <= 0 || h <= 0) return;

  const insetX = Math.round(w * inset);
  const insetY = Math.round(h * inset);

  const horizontal = (top: number) => {
    const x0 = x + insetX;
    const x1 = x + w - 1 - insetX;
    if (x1 < x0) return;
    if (style === "solid") {
      fb.fillRect(x0, top, x1 - x0 + 1, weight, color);
    } else {
      const { dash, gap } = dashFor(style);
      for (let k = 0; k < weight; k += 1) {
        dashedLine(fb, x0, top + k, x1, top + k, color, dash, gap);
      }
    }
  };

  const vertical = (left: number) => {
    const y0 = y + insetY;
    const y1 = y + h - 1 - insetY;
    if (y1 < y0) return;
    if (style === "solid") {
      fb.fillRect(left, y0, weight, y1 - y0 + 1, color);
    } else {
      const { dash, gap } = dashFor(style);
      for (let k = 0; k < weight; k += 1) {
        dashedLine(fb, left + k, y0, left + k, y1, color, dash, gap);
      }
    }
  };

  for (const edge of frame.edges) {
    if (edge === "top") horizontal(y);
    else if (edge === "bottom") horizontal(y + h - weight);
    else if (edge === "left") vertical(x);
    else if (edge === "right") vertical(x + w - weight);
  }
}
