import { z } from "zod";
import type { FrameBuffer } from "@/core/frame";
import { BLACK, RED, WHITE, type PaletteIndex } from "@/core/palette";
import {
  drawText,
  reservedHeight,
  textElementSchema,
  textStyleSchema,
  type ModuleReporter,
  type TextElement,
  type TextStyle,
} from "../text";
import type { PixelRect } from "../types";

/** Breathing room inside every module box, so neighbours do not touch. */
export const PAD = 4;

export function inner(rect: PixelRect): PixelRect {
  return {
    x: rect.x + PAD,
    y: rect.y + PAD,
    w: Math.max(0, rect.w - PAD * 2),
    h: Math.max(0, rect.h - PAD * 2),
  };
}

/** Clear a module's own area. Modules never inherit neighbours' ink. */
export function clearModule(fb: FrameBuffer, rect: PixelRect): void {
  fb.fillRect(rect.x, rect.y, rect.w, rect.h, WHITE);
}

/**
 * The unavailable and stale wording, as editable text like everything else.
 *
 * The copy is the owner's to change, but the CONTRACT is not: a module still has
 * to render an explicit unavailable state rather than a zero, a dash, or a
 * blank. Panel ink is frozen for hours and a fabricated value reads as
 * authoritative the whole time. Hiding this text hides the words, never the
 * fact that the data is missing, because an empty module is itself a visible
 * statement that nothing was known.
 */
export function unavailableShape(defaults: { title: string; note: string }) {
  return {
    unavailableTitle: textElementSchema({
      text: defaults.title,
      maxLength: 60,
      style: { size: 15, weight: "bold" },
    }).default({}),
    unavailableNote: textElementSchema({
      text: defaults.note,
      maxLength: 60,
      style: { size: 13 },
    }).default({}),
  };
}

export interface UnavailableOptions {
  unavailableTitle: TextElement;
  unavailableNote: TextElement;
}

/** Draw the unavailable state in the attention colour. */
export function renderUnavailable(
  fb: FrameBuffer,
  rect: PixelRect,
  options: UnavailableOptions,
  reporter?: ModuleReporter,
): void {
  const box = inner(rect);
  const title = drawText(
    fb,
    box,
    options.unavailableTitle,
    RED,
    "unavailableTitle",
    { wrap: true, ...(reporter ? { reporter } : {}) },
  );

  const used = title.nextY - box.y;
  const remaining = box.h - used;
  if (remaining >= reservedHeight(options.unavailableNote)) {
    drawText(
      fb,
      { x: box.x, y: title.nextY, w: box.w, h: remaining },
      options.unavailableNote,
      BLACK,
      "unavailableNote",
      { wrap: true, ...(reporter ? { reporter } : {}) },
    );
  }
}

/** The "this reading is out of date" label, as editable text. */
export function staleShape(text: string) {
  return {
    staleLabel: textElementSchema({
      text,
      maxLength: 60,
      style: { size: 13 },
    }).default({}),
  };
}

/**
 * Source provenance.
 *
 * Provenance is styleable but NOT editable, and it is off by default. Both
 * halves of that are deliberate:
 *
 *   * Off by default because provenance belongs in Tower diagnostics. The
 *     panel is 400x300 and the line it used to force on was
 *     "<place> approx. · Open-Meteo", which the owner never asked for and does
 *     not want.
 *   * Not editable because it is a claim about where a number came from. A
 *     free-text provenance field would let the panel state a source that did
 *     not supply the data, which is the one kind of text in this product that
 *     must stay the machine's to write.
 */
export function provenanceShape(style: Partial<TextStyle> = { size: 11 }) {
  return {
    showProvenance: z.boolean().default(false),
    provenanceStyle: textStyleSchema(style).default({}),
  };
}

export interface ProvenanceOptions {
  showProvenance: boolean;
  provenanceStyle: TextStyle;
}

/**
 * Draw the source's own provenance string, if the user asked for it.
 * Returns the y to continue at, unchanged when nothing was drawn.
 */
export function renderProvenance(
  fb: FrameBuffer,
  box: PixelRect,
  options: ProvenanceOptions,
  text: string,
  colour: PaletteIndex,
  reporter?: ModuleReporter,
): number {
  if (!options.showProvenance || text.trim().length === 0) return box.y;
  return drawText(
    fb,
    box,
    { text, visible: true, style: options.provenanceStyle },
    colour,
    "provenance",
    reporter ? { reporter } : {},
  ).nextY;
}

export { BLACK, RED };
