import { z } from "zod";
import { BLACK, RED, WHITE, YELLOW, type PaletteIndex } from "@/core/palette";
import {
  DEFAULT_EXPRESSION,
  type Expression,
} from "@/core/theme";
import {
  accentBudget,
  fillBandDither,
  fillRectDither,
  scrubIsolatedAccents,
  type DitherStyle,
} from "../dither";
import {
  drawText,
  reservedHeight,
  textElementSchema,
} from "../text";
import type { ModuleDefinition, PixelRect } from "../types";
import { LAYOUT_VARIANT_TAG } from "../types";
import { clearModule, inner } from "./common";

/**
 * Headline — the editorial hero.
 *
 * A manual headline, an optional kicker above it and an optional subline below,
 * set large over a dithered colour field. It is the module that makes the panel
 * look like a printed page rather than a readout: big type, a warm bar, air.
 *
 * Nothing here is data-bound. The words are the owner's, typed and fixed, which
 * is the honest thing for a headline: a machine that guessed a headline would
 * be writing copy. Overflow is handled the standard way — drawn as far as it
 * goes, marked in red on the panel, reported to the designer — never silently
 * cut.
 *
 * The colour field is laid down by the shared dither engine, so it obeys the
 * dashboard's Expression: its brush, its texture, and how much warm ink the
 * colour-use stance will spend. A "Black & white" dashboard sets the same
 * headline in ink on paper; an "Expressive" one gives it a deep, warm bar.
 */

export const HEADLINE_VARIANTS = [
  "underline",
  "banner",
  "sidebar",
  "wash",
] as const;
export type HeadlineVariant = (typeof HEADLINE_VARIANTS)[number];

export const HEADLINE_PALETTES = ["warm", "sun", "ink"] as const;
export type HeadlinePalette = (typeof HEADLINE_PALETTES)[number];

export const HeadlineOptions = z.object({
  /** How the type and the colour field are arranged. Chosen by thumbnail. */
  variant: z.enum(HEADLINE_VARIANTS).default("underline").describe(LAYOUT_VARIANT_TAG),
  /** Which pigment the field is laid in, before the colour-use stance is applied. */
  palette: z.enum(HEADLINE_PALETTES).default("warm"),
  kicker: textElementSchema({
    text: "AUJOURD'HUI",
    maxLength: 40,
    style: { family: "plexmono", size: 13, weight: "bold" },
  }).default({}),
  headline: textElementSchema({
    text: "Bonjour",
    maxLength: 60,
    style: { family: "poppins", size: 34, weight: "bold" },
  }).default({}),
  subline: textElementSchema({
    text: "Une bonne journée à la maison",
    maxLength: 80,
    style: { size: 15 },
  }).default({}),
});
export type HeadlineOptions = z.infer<typeof HeadlineOptions>;

/** The pigment this palette resolves to, honouring the colour-use stance. */
function fieldPigment(
  palette: HeadlinePalette,
  expression: Expression,
): PaletteIndex {
  if (expression.colourUse === "blackwhite") return BLACK;
  if (palette === "sun") return YELLOW;
  if (palette === "ink") return BLACK;
  return RED;
}

function styleOf(expression: Expression): DitherStyle {
  return { brush: expression.brush, texture: expression.pixelTexture };
}

/** The coverage a "full" field gets under the current stance, for a pigment. */
function fieldCoverage(pigment: PaletteIndex, expression: Expression): number {
  if (pigment === BLACK) {
    // Ink fields are read directly, not through the accent budget: a black &
    // white dashboard still deserves a strong bar. Yellow-on-ink would vanish,
    // so ink never goes fully solid — it keeps a tooth so knockout type reads.
    return expression.colourUse === "expressive" ? 0.82 : 0.68;
  }
  const budget = accentBudget(expression.colourUse);
  return budget.ceiling;
}

export const headline: ModuleDefinition<HeadlineOptions, never> = {
  type: "headline",
  label: "Headline",
  description:
    "Big editorial type — a headline, an optional kicker and subline — set over a dithered colour field. The words are yours; the colour follows the dashboard's Expression.",
  schema: HeadlineOptions,
  defaultOptions: HeadlineOptions.parse({}),
  defaultSpan: { w: 8, h: 2 },
  minSpan: { w: 4, h: 2 },
  maxSpan: { w: 8, h: 4 },
  sourceBinding: "none",

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const expression = ctx.theme?.expression ?? DEFAULT_EXPRESSION;
    const style = styleOf(expression);
    const pigment = fieldPigment(options.palette, expression);
    const coverage = fieldCoverage(pigment, expression);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};
    const box = inner(rect);

    const drawStack = (
      area: PixelRect,
      opts: {
        kickerColour: PaletteIndex;
        headlineColour: PaletteIndex;
        sublineColour: PaletteIndex;
        kickerBg?: PaletteIndex;
        headlineBg?: PaletteIndex;
      },
    ): void => {
      let y = area.y;
      const below = (): PixelRect => ({
        x: area.x,
        y,
        w: area.w,
        h: Math.max(0, area.h - (y - area.y)),
      });
      if (reservedHeight(options.kicker) > 0) {
        y = drawText(fb, below(), options.kicker, opts.kickerColour, "kicker", {
          ...report,
          ...(opts.kickerBg !== undefined ? { background: opts.kickerBg } : {}),
        }).nextY;
      }
      y = drawText(fb, below(), options.headline, opts.headlineColour, "headline", {
        wrap: true,
        ...report,
        ...(opts.headlineBg !== undefined ? { background: opts.headlineBg } : {}),
      }).nextY;
      if (reservedHeight(options.subline) > 0) {
        drawText(fb, below(), options.subline, opts.sublineColour, "subline", {
          wrap: true,
          ...report,
        });
      }
    };

    switch (options.variant) {
      case "banner": {
        // A full-width colour band across the top holds the kicker in knockout
        // paper; the headline is set large in ink on the paper below.
        const bandH = Math.min(
          box.h - 4,
          Math.max(reservedHeight(options.kicker) + 8, Math.floor(rect.h * 0.32)),
        );
        fillRectDither(
          fb,
          { x: rect.x, y: rect.y, w: rect.w, h: bandH + (box.y - rect.y) },
          pigment,
          coverage,
          style,
        );
        // Kicker centred and knocked out of the band.
        const kickerH = reservedHeight(options.kicker);
        if (kickerH > 0) {
          drawText(
            fb,
            { x: box.x, y: rect.y + Math.floor(((bandH + (box.y - rect.y)) - kickerH) / 2), w: box.w, h: kickerH },
            { ...options.kicker, style: { ...options.kicker.style, align: "center" } },
            WHITE,
            "kicker",
            { ...report, background: pigment },
          );
        }
        let y = rect.y + bandH + (box.y - rect.y) + 4;
        y = drawText(
          fb,
          { x: box.x, y, w: box.w, h: Math.max(0, box.y + box.h - y) },
          { ...options.headline, style: { ...options.headline.style, align: "center" } },
          BLACK,
          "headline",
          { wrap: true, ...report },
        ).nextY;
        if (reservedHeight(options.subline) > 0) {
          drawText(
            fb,
            { x: box.x, y: y + 2, w: box.w, h: Math.max(0, box.y + box.h - y - 2) },
            { ...options.subline, style: { ...options.subline.style, align: "center" } },
            BLACK,
            "subline",
            { wrap: true, ...report },
          );
        }
        break;
      }

      case "sidebar": {
        // A tall colour block down the left; the type sits in ink on the right.
        const blockW = Math.max(24, Math.floor(rect.w * 0.34));
        fillRectDither(
          fb,
          { x: rect.x, y: rect.y, w: blockW, h: rect.h },
          pigment,
          coverage,
          style,
        );
        drawStack(
          {
            x: rect.x + blockW + 8,
            y: box.y,
            w: rect.x + rect.w - (rect.x + blockW + 8) - 4,
            h: box.h,
          },
          { kickerColour: BLACK, headlineColour: BLACK, sublineColour: BLACK },
        );
        break;
      }

      case "wash": {
        // The whole tile a light grain of colour, so it reads as tinted paper;
        // the type in ink over it, with a solid rule under the headline.
        const washCoverage = Math.min(0.24, coverage * 0.3);
        fillRectDither(fb, rect, pigment, washCoverage, style);
        drawStack(
          { x: box.x, y: box.y, w: box.w, h: box.h },
          { kickerColour: BLACK, headlineColour: BLACK, sublineColour: BLACK },
        );
        break;
      }

      case "underline":
      default: {
        // Kicker, big headline, then a bold colour bar swept under it like a
        // marker stroke, then the subline. The default, and the most editorial.
        let y = box.y;
        if (reservedHeight(options.kicker) > 0) {
          y = drawText(fb, { x: box.x, y, w: box.w, h: box.h }, options.kicker, BLACK, "kicker", report).nextY;
        }
        const headY = y;
        y = drawText(
          fb,
          { x: box.x, y, w: box.w, h: Math.max(0, box.y + box.h - y) },
          options.headline,
          BLACK,
          "headline",
          { wrap: true, ...report },
        ).nextY;
        const barTop = y + 2;
        const barH = Math.max(6, Math.floor(rect.h * 0.16));
        if (barTop + barH <= box.y + box.h) {
          fillBandDither(
            fb,
            { x: box.x, y: barTop, w: box.w, h: barH },
            pigment,
            { from: coverage, to: Math.max(0, coverage - 0.28), axis: "x", style },
          );
          y = barTop + barH + 4;
        }
        if (reservedHeight(options.subline) > 0 && y < box.y + box.h) {
          drawText(
            fb,
            { x: box.x, y, w: box.w, h: Math.max(0, box.y + box.h - y) },
            options.subline,
            BLACK,
            "subline",
            { wrap: true, ...report },
          );
        }
        void headY;
        break;
      }
    }

    // Knockout type and ink rules sit on top of warm dithered fields; where
    // they clip an accent cell to a single pixel, this removes it, so the
    // finished tile honours the 2 px rule and not merely each fill.
    scrubIsolatedAccents(fb, rect);
  },
};
