import { z } from "zod";
import type { FrameBuffer } from "@/core/frame";
import { RED, WHITE, paletteName, type PaletteIndex } from "@/core/palette";
import {
  FONT_SIZES,
  clean,
  fitText,
  isFontSize,
  measureText,
  unsupportedSymbols,
  wrapText,
  type FontAtlas,
  type FontSize,
} from "@/core/font";
import {
  COLOUR_TOKENS,
  DEFAULT_THEME,
  STYLE_FAMILIES,
  STYLE_WEIGHTS,
  colourFor,
  contrastVerdict,
  resolveFamily,
  resolveWeight,
  type ColourToken,
  type ContrastVerdict,
  type StyleFamily,
  type StyleWeight,
} from "@/core/theme";
import { font } from "./fonts";
import type { PixelRect } from "./types";

/**
 * Editable, styleable, hideable text.
 *
 * Every string the panel shows is one of these, so three separate promises
 * hold at once:
 *
 *   * The owner can change the words. Nothing the panel says is welded into a
 *     module's source.
 *   * The owner can hide the words without deleting the module that draws them,
 *     and without inventing a per-word object to do it.
 *   * Nothing is silently clipped. Text that does not fit its box is drawn as
 *     far as it goes, marked on the panel in the attention colour, and
 *     reported back to the designer as a warning.
 *
 * Typography lives on a text ROLE rather than on a module, so a module can
 * have a large heading and a small footnote without either of them being a
 * separate module.
 */

export const TEXT_ALIGNMENTS = ["left", "center", "right"] as const;
export type TextAlignment = (typeof TEXT_ALIGNMENTS)[number];

/** Marker used on both schemas so the inspector can give them a real widget. */
export const TEXT_STYLE_TAG = "text-style";
export const TEXT_ELEMENT_TAG = "text-element";

const FontSizeSchema = z
  .number()
  .int()
  .refine(isFontSize, {
    message: `Font size must be one of ${FONT_SIZES.join(", ")} px, because those are the sizes with a committed glyph atlas`,
  });

/** Extra pixels between wrapped lines, on top of the face's own line height. */
export const MIN_LINE_SPACING = 0;
export const MAX_LINE_SPACING = 12;

export interface TextStyle {
  /** A concrete family, or "inherit" to follow the dashboard theme. */
  family: StyleFamily;
  size: FontSize;
  /** A concrete weight, or "inherit" to follow the dashboard theme. */
  weight: StyleWeight;
  align: TextAlignment;
  lineSpacing: number;
  /**
   * Which pigment this role is drawn in. "inherit" means the colour the module
   * chose for it, which is why it is the default: adding this field changed no
   * existing panel.
   */
  colour: ColourToken;
}

export function textStyleSchema(defaults: Partial<TextStyle> = {}) {
  return z
    .object({
      family: z.enum(STYLE_FAMILIES).default(defaults.family ?? "inter"),
      size: FontSizeSchema.default(defaults.size ?? 15),
      weight: z.enum(STYLE_WEIGHTS).default(defaults.weight ?? "regular"),
      align: z.enum(TEXT_ALIGNMENTS).default(defaults.align ?? "left"),
      lineSpacing: z
        .number()
        .int()
        .min(MIN_LINE_SPACING)
        .max(MAX_LINE_SPACING)
        .default(defaults.lineSpacing ?? 2),
      colour: z.enum(COLOUR_TOKENS).default(defaults.colour ?? "inherit"),
    })
    .describe(TEXT_STYLE_TAG);
}

export const TextStyleSchema = textStyleSchema();

export interface TextElement {
  text: string;
  visible: boolean;
  style: TextStyle;
}

/**
 * A text role's schema, with this role's own default wording and styling
 * baked into it. The defaults are in the schema rather than beside it so
 * `schema.parse({})` is the single source of a module's default options.
 */
export function textElementSchema(options: {
  text: string;
  /** Hard character ceiling. The inspector enforces the same number. */
  maxLength?: number;
  visible?: boolean;
  style?: Partial<TextStyle>;
}) {
  return z
    .object({
      text: z.string().max(options.maxLength ?? 120).default(options.text),
      visible: z.boolean().default(options.visible ?? true),
      style: textStyleSchema(options.style).default({}),
    })
    .describe(TEXT_ELEMENT_TAG);
}

/**
 * Substitute {placeholders} in an editable string with data-bound values.
 *
 * This is what lets every word be the owner's while the numbers stay the
 * machine's. "{low} à {high} {unit}" is a sentence he can rewrite, reorder or
 * empty out; low, high and unit are still whatever the source actually
 * reported, and there is no way to type a temperature into them.
 *
 * An unknown placeholder is left standing rather than silently blanked, so a
 * typo shows up on the panel as {hihg} instead of as a missing number.
 */
export function fillTemplate(
  template: string,
  values: Record<string, string | number>,
): string {
  return template.replace(/\{(\w+)\}/g, (match, key: string) =>
    Object.prototype.hasOwnProperty.call(values, key)
      ? String(values[key])
      : match,
  );
}

/** A text element with its placeholders already resolved. */
export function filled(
  element: TextElement,
  values: Record<string, string | number>,
): TextElement {
  return { ...element, text: fillTemplate(element.text, values) };
}

/**
 * The atlas a style needs.
 *
 * Inheritance is normally resolved once at the render boundary, so by the time
 * a style gets here its family is concrete. The fallback to the default theme
 * is for a module rendered on its own, outside a dashboard: better a readable
 * default face than a throw from deep inside a measurement.
 */
export function atlasFor(style: TextStyle): FontAtlas {
  return font(
    resolveFamily(style.family, DEFAULT_THEME),
    resolveWeight(style.weight, DEFAULT_THEME),
    style.size,
  );
}

/** Baseline-to-baseline step for a style. */
export function lineStep(style: TextStyle): number {
  return atlasFor(style).lineHeight + style.lineSpacing;
}

export interface OverflowFact {
  /** Which text role overflowed, e.g. "heading". */
  role: string;
  /** The full text the user asked for, so the warning can quote it. */
  text: string;
  kind: "width" | "height";
  neededPx: number;
  availablePx: number;
  /**
   * Which module instance it happened in. Modules do not know their own id, so
   * renderDashboard stamps this on as the fact goes past; a module rendered on
   * its own simply leaves it unset.
   */
  moduleId?: string;
  moduleType?: string;
}

/** A colour combination the panel cannot show. */
export interface ContrastFact {
  role: string;
  verdict: Exclude<ContrastVerdict, null>;
  /** Pigment names, so the warning can say "white on white". */
  foreground: string;
  background: string;
  moduleId?: string;
  moduleType?: string;
}

/**
 * Something true about this role's layout that the designer should know, and
 * that is not an error.
 *
 * `line-gap-inert` is a box only tall enough for one line at this size. The
 * line gap is the distance between lines, so in such a box the control does
 * nothing at all. Saying so is the honest fix; forcing a redraw or fudging the
 * fits arithmetic would hide a true fact about the tile.
 *
 * `unsupported-symbols` names the emoji the panel has no pictogram for.
 *
 * `empty-role` is a role that was asked to draw and had no words, where the
 * blank it leaves could be mistaken for something else having happened.
 */
export type LayoutNoteKind =
  | "line-gap-inert"
  | "unsupported-symbols"
  | "empty-role";

export interface LayoutNote {
  role: string;
  kind: LayoutNoteKind;
  detail: string;
  /** Numbers, so a line-gap note can be specific instead of vague. */
  lineHeightPx?: number;
  stepPx?: number;
  availablePx?: number;
  /** The symbols with no pictogram, so the note can show them. */
  symbols?: string[];
  moduleId?: string;
  moduleType?: string;
}

/** Modules push facts here; the designer surfaces them. */
export interface ModuleReporter {
  overflow(fact: OverflowFact): void;
  /** Optional so an older test double is still a valid reporter. */
  contrast?(fact: ContrastFact): void;
  note?(fact: LayoutNote): void;
}

export interface LaidOutText {
  lines: string[];
  atlas: FontAtlas;
  step: number;
  /** Widest line, in pixels. */
  width: number;
  /** Total height of every line, in pixels. */
  height: number;
  /** Lines that fit the box vertically. */
  visibleLines: number;
  /**
   * Whether a second line could fit this box at this size at ANY line gap,
   * including the smallest one the control offers. False means the control is
   * inert here: no value it can take will move a pixel.
   */
  secondLineFits: boolean;
  /** Symbols in this text the panel has no pictogram for. */
  unsupported: string[];
  overflow: OverflowFact[];
}

export interface LayoutOptions {
  /** Wrap on words to the box width. Off means one line, truncated. */
  wrap?: boolean;
  /** Stop after this many lines even if the box is taller. */
  maxLines?: number;
  /** Height available, when it differs from the box (e.g. under a heading). */
  height?: number;
}

/**
 * Lay a text element out inside a box, without drawing anything.
 *
 * Returns every line, how many of them fit, and what overflowed. Nothing here
 * decides to hide text: the caller draws what fits and the overflow facts go
 * to the designer either way.
 */
export function layoutText(
  element: TextElement,
  box: PixelRect,
  role: string,
  options: LayoutOptions = {},
): LaidOutText | null {
  if (!element.visible) return null;
  const text = clean(element.text);
  if (text.length === 0) return null;

  const atlas = atlasFor(element.style);
  const step = lineStep(element.style);
  const height = options.height ?? box.h;
  const overflow: OverflowFact[] = [];

  let lines: string[];
  if (options.wrap) {
    const wrapped = wrapText(atlas, text, box.w);
    lines = wrapped.lines;
    if (wrapped.hardBroken) {
      // A single word wider than the box. It was broken mid-word to show it
      // at all, which is never what anybody meant.
      const widest = Math.max(...lines.map((line) => measureText(atlas, line)), 0);
      overflow.push({
        role,
        text,
        kind: "width",
        neededPx: Math.max(widest, box.w + 1),
        availablePx: box.w,
      });
    }
  } else {
    lines = [text];
    const width = measureText(atlas, text);
    if (width > box.w) {
      overflow.push({ role, text, kind: "width", neededPx: width, availablePx: box.w });
    }
  }

  if (lines.length === 0) return null;

  const width = Math.max(...lines.map((line) => measureText(atlas, line)), 0);
  const totalHeight = (lines.length - 1) * step + atlas.lineHeight;

  // How many whole lines fit. A line whose descenders would leave the box is
  // not counted: half a line of ink on frozen e-paper reads as damage.
  let fits = 0;
  while (fits < lines.length && fits * step + atlas.lineHeight <= height) {
    fits += 1;
  }
  if (options.maxLines !== undefined) fits = Math.min(fits, options.maxLines);

  if (fits < lines.length) {
    overflow.push({
      role,
      text,
      kind: "height",
      neededPx: totalHeight,
      availablePx: height,
    });
  }

  return {
    lines,
    atlas,
    step,
    width,
    height: totalHeight,
    visibleLines: Math.max(0, fits),
    // Measured at the SMALLEST gap, not the current one. At a large gap the
    // second line may be pushed out, but turning the gap down would bring it
    // back, so the control is not inert and saying it was would be false.
    secondLineFits: atlas.lineHeight * 2 + MIN_LINE_SPACING <= height,
    unsupported: unsupportedSymbols(atlas, text),
    overflow,
  };
}

/** Width of the attention bar painted beside text that did not fit. */
export const OVERFLOW_MARK_WIDTH = 2;

/**
 * Paint the "there is more text than this" mark.
 *
 * Deliberately in the attention colour and on the panel itself, not only in
 * the designer. E-paper holds its image for hours with the power off; a
 * quietly truncated sentence would sit there looking complete the whole time.
 */
function markOverflow(fb: FrameBuffer, x: number, y: number, h: number): void {
  fb.fillRect(x, y, OVERFLOW_MARK_WIDTH, Math.max(1, h), RED);
}

export interface DrawTextResult {
  /** Y coordinate just past the last line drawn. */
  nextY: number;
  overflow: OverflowFact[];
}

/**
 * Draw a text element inside a box and report what did not fit.
 *
 * Returns the y to continue at, so modules can stack roles without each one
 * hardcoding the height of the one above it.
 */
export function drawText(
  fb: FrameBuffer,
  box: PixelRect,
  element: TextElement,
  colour: PaletteIndex,
  role: string,
  options: LayoutOptions & {
    reporter?: ModuleReporter;
    /**
     * What this text sits on. White everywhere in this build, because every
     * module clears its own rectangle to paper before drawing.
     */
    background?: PaletteIndex;
  } = {},
): DrawTextResult {
  const laid = layoutText(element, box, role, options);
  if (!laid) return { nextY: box.y, overflow: [] };

  const { atlas, step, lines, visibleLines } = laid;
  const align = element.style.align;
  // The module's own colour for this element is the fallback; the role's token
  // wins when it names a pigment.
  const pigment = colourFor(element.style.colour, colour);
  const background = options.background ?? WHITE;

  for (let i = 0; i < visibleLines; i += 1) {
    const line = lines[i] as string;
    const y = box.y + i * step;
    const lineWidth = measureText(atlas, line);
    const tooWide = lineWidth > box.w;
    // Leave room for the mark so it never sits on top of a letter.
    const room = tooWide ? box.w - OVERFLOW_MARK_WIDTH - 1 : box.w;
    const drawn = tooWide ? fitText(atlas, line, Math.max(0, room)) : line;

    let penX = box.x;
    if (align === "right") penX = box.x + box.w;
    else if (align === "center") penX = box.x + Math.floor(box.w / 2);

    fb.drawText(atlas, penX, y, drawn, pigment, {
      ...(align === "left" ? {} : { align }),
    });

    if (tooWide) {
      markOverflow(fb, box.x + box.w - OVERFLOW_MARK_WIDTH, y, atlas.lineHeight);
    }
  }

  // Height overflow gets its own mark at the bottom right of the box, so a
  // block that lost a whole line is as visible as one that lost a word.
  if (visibleLines < lines.length) {
    if (visibleLines > 0) {
      const lastY = box.y + (visibleLines - 1) * step;
      markOverflow(
        fb,
        box.x + box.w - OVERFLOW_MARK_WIDTH,
        lastY + atlas.lineHeight - 3,
        3,
      );
    } else {
      /*
       * Not one line fitted. This is the worst case and it used to be the
       * silent one: a 34 px countdown in a two by one tile drew its label and
       * then nothing, which on frozen ink reads as a countdown with no number
       * rather than as a tile that could not hold it. Mark the full height of
       * the box, so the panel itself says something is missing here.
       */
      markOverflow(
        fb,
        box.x + box.w - OVERFLOW_MARK_WIDTH,
        box.y,
        Math.max(3, options.height ?? box.h),
      );
    }
  }

  const reporter = options.reporter;
  if (reporter) {
    for (const fact of laid.overflow) reporter.overflow(fact);

    const verdict = contrastVerdict(pigment, background);
    if (verdict && reporter.contrast) {
      reporter.contrast({
        role,
        verdict,
        foreground: paletteName(pigment),
        background: paletteName(background),
      });
    }

    if (laid.unsupported.length > 0 && reporter.note) {
      const unique = [...new Set(laid.unsupported)];
      reporter.note({
        role,
        kind: "unsupported-symbols",
        detail: `${unique.join(" ")} ${unique.length === 1 ? "has" : "have"} no panel pictogram, so ${unique.length === 1 ? "it is" : "they are"} drawn as a boxed question mark rather than dropped.`,
        symbols: unique,
      });
    }

    // A wrapping role in a box that cannot hold a second line: the line gap is
    // a real control with nothing to move. Only wrapping roles, because a role
    // that is one line by design has no expectation to violate.
    if (options.wrap && !laid.secondLineFits && reporter.note) {
      reporter.note({
        role,
        kind: "line-gap-inert",
        detail: `This box fits one line at ${element.style.size} px, so the line gap has nothing to move. Make the tile taller, or set this role smaller.`,
        lineHeightPx: atlas.lineHeight,
        stepPx: step,
        availablePx: options.height ?? box.h,
      });
    }
  }

  return {
    nextY: box.y + visibleLines * step,
    overflow: laid.overflow,
  };
}

/**
 * How tall a role is when it is visible, and zero when it is not.
 *
 * Modules use this to stack: hiding a heading should close the gap it left,
 * not leave a hole where it used to be.
 */
export function reservedHeight(element: TextElement): number {
  if (!element.visible || clean(element.text).length === 0) return 0;
  return atlasFor(element.style).lineHeight + element.style.lineSpacing;
}
