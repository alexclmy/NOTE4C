import { z } from "zod";
import { BLACK, RED, type PaletteIndex } from "@/core/palette";
import { clean, measureText } from "@/core/font";
import { ColourTokenSchema, colourFor } from "@/core/theme";
import {
  atlasFor,
  drawText,
  reservedHeight,
  textElementSchema,
  textStyleSchema,
  type TextStyle,
} from "../text";
import type { ModuleDefinition, PixelRect } from "../types";
import { clearModule, inner } from "./common";
import type { FrameBuffer } from "@/core/frame";

/**
 * A list the owner writes.
 *
 * Not a feed and not a mirror of anything: the rows are typed in the designer
 * and the panel shows exactly them. That is the point. A shopping list, the
 * week's chores, what to pick up on the way home — the things a fridge note is
 * for, which no adapter on this machine knows about.
 *
 * Three things it deliberately does NOT do.
 *
 *   * It never invents a row. An empty list draws its empty state, in words
 *     the owner chose, rather than a placeholder or a dash.
 *   * It never hides a row it was given. Rows past the visible maximum are
 *     reported to the designer and marked in the attention colour on the
 *     panel, because frozen ink that quietly stops at six items looks like a
 *     list of six items.
 *   * A checkbox is a drawing. The NOTE4C has no buttons and this build has no
 *     way to tick one; the box is there because a list of chores reads better
 *     with boxes, and the inspector says so in as many words.
 */

/** Marker so the inspector gives the rows a real editor. */
export const LIST_ROWS_TAG = "list-rows";

export const LIST_MARKERS = ["bullet", "numbered", "checkbox"] as const;
export type ListMarker = (typeof LIST_MARKERS)[number];

export const MAX_LIST_ROWS = 12;

export const ListRowSchema = z.object({
  text: z.string().max(60).default(""),
  /**
   * A hidden row keeps its wording and its place in the order. Same contract
   * as a hidden module: "not this week" is not "delete it".
   */
  visible: z.boolean().default(true),
});
export type ListRow = z.infer<typeof ListRowSchema>;

export const ListOptions = z.object({
  title: textElementSchema({
    text: "LISTE",
    maxLength: 40,
    style: { size: 15, weight: "bold" },
  }).default({}),
  rows: z
    .array(ListRowSchema)
    .max(MAX_LIST_ROWS)
    .default([])
    .describe(LIST_ROWS_TAG),
  marker: z.enum(LIST_MARKERS).default("bullet"),
  /** Markers and checkboxes can take an accent without the words following. */
  markerColour: ColourTokenSchema.default("inherit"),
  /**
   * How many rows may be drawn. Separate from how many exist, so a long list
   * can be kept in the dashboard and shown a few at a time, and so the panel
   * can say out loud that there are more.
   */
  maxVisibleRows: z.number().int().min(1).max(MAX_LIST_ROWS).default(6),
  rowStyle: textStyleSchema({ size: 13 }).default({}),
  emptyText: textElementSchema({
    text: "Rien pour l'instant",
    maxLength: 60,
    style: { size: 13 },
  }).default({}),
  /** Drawn when rows exist beyond the visible maximum. {count} is the rest. */
  moreText: textElementSchema({
    text: "+{count} de plus",
    maxLength: 40,
    style: { size: 11 },
  }).default({}),
});
export type ListOptions = z.infer<typeof ListOptions>;

/** The rows that would actually be drawn: visible, and not blank. */
export function listedRows(options: ListOptions): ListRow[] {
  return options.rows.filter(
    (row) => row.visible && clean(row.text).length > 0,
  );
}

/** Width the marker column needs, so every row's text starts in line. */
export function markerWidth(options: ListOptions, count: number): number {
  const atlas = atlasFor(options.rowStyle);
  if (options.marker === "numbered") {
    let widest = 0;
    for (let index = 1; index <= Math.max(1, count); index += 1) {
      widest = Math.max(widest, measureText(atlas, `${index}.`));
    }
    return widest + 4;
  }
  return boxSide(options.rowStyle) + 5;
}

function boxSide(style: TextStyle): number {
  const atlas = atlasFor(style);
  // Square, odd-ish, and never taller than the line it sits on.
  return Math.max(5, Math.min(11, atlas.lineHeight - 6));
}

function drawMarker(
  fb: FrameBuffer,
  x: number,
  y: number,
  index: number,
  options: ListOptions,
  colour: PaletteIndex,
): void {
  const atlas = atlasFor(options.rowStyle);

  if (options.marker === "numbered") {
    fb.drawText(atlas, x, y, `${index + 1}.`, colour);
    return;
  }

  const side = boxSide(options.rowStyle);
  const top = y + Math.max(0, Math.floor((atlas.lineHeight - side) / 2));

  if (options.marker === "checkbox") {
    fb.strokeRect(x, top, side, side, colour);
    return;
  }

  // A bullet: a small filled square, which survives the panel's dither better
  // than a circle of this size does.
  const dot = Math.max(3, side - 4);
  fb.fillRect(
    x + Math.floor((side - dot) / 2),
    top + Math.floor((side - dot) / 2),
    dot,
    dot,
    colour,
  );
}

export const list: ModuleDefinition<ListOptions, never> = {
  type: "list",
  label: "List",
  description:
    "A list you write yourself: chores, shopping, reminders for the household. Bullets, numbers or checkboxes. The checkbox is a drawing only — the panel has no buttons, so nothing can be ticked from it.",
  schema: ListOptions,
  defaultOptions: ListOptions.parse({}),
  defaultSpan: { w: 4, h: 3 },
  minSpan: { w: 2, h: 1 },
  maxSpan: { w: 8, h: 6 },
  sourceBinding: "none",

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    const titleHeight = reservedHeight(options.title);
    if (titleHeight > 0) {
      drawText(fb, box, options.title, BLACK, "title", report);
    }

    const body: PixelRect = {
      x: box.x,
      y: box.y + titleHeight,
      w: box.w,
      h: Math.max(0, box.h - titleHeight),
    };

    const rows = listedRows(options);
    if (rows.length === 0) {
      // An empty list is a fact about the list, not a failure, so it is drawn
      // in ink rather than in the attention colour. What it is not is a guess.
      drawText(fb, body, options.emptyText, BLACK, "emptyText", {
        wrap: true,
        ...report,
      });
      return;
    }

    const atlas = atlasFor(options.rowStyle);
    const step = atlas.lineHeight + options.rowStyle.lineSpacing;
    const gutter = markerWidth(options, rows.length);
    const marker = colourFor(options.markerColour, BLACK);

    let drawnRows = 0;
    let y = body.y;
    while (drawnRows < rows.length && drawnRows < options.maxVisibleRows) {
      if (y + atlas.lineHeight > body.y + body.h) break;
      const row = rows[drawnRows] as ListRow;
      drawMarker(fb, body.x, y, drawnRows, options, marker);
      drawText(
        fb,
        { x: body.x + gutter, y, w: Math.max(0, body.w - gutter), h: atlas.lineHeight },
        { text: row.text, visible: true, style: options.rowStyle },
        BLACK,
        "rows",
        report,
      );
      y += step;
      drawnRows += 1;
    }

    const hidden = rows.length - drawnRows;
    if (hidden <= 0) return;

    // There are more rows than the panel is showing. Say so on the panel, in
    // The owner's own wording, and tell the designer the exact numbers.
    const moreHeight = reservedHeight(options.moreText);
    if (moreHeight > 0 && y + moreHeight <= body.y + body.h) {
      drawText(
        fb,
        { x: body.x, y, w: body.w, h: moreHeight },
        {
          ...options.moreText,
          text: options.moreText.text.replace("{count}", String(hidden)),
        },
        RED,
        "moreText",
        report,
      );
    } else {
      // No room even for the "+N more" line: fall back to the same attention
      // mark the text layer uses, so the truncation is never invisible.
      fb.fillRect(box.x + box.w - 2, box.y + box.h - 3, 2, 3, RED);
    }

    if (reporter) {
      reporter.overflow({
        role: "rows",
        text: `${rows.length} rangées configurées`,
        kind: "height",
        neededPx: rows.length * step,
        availablePx: body.h,
      });
    }
  },
};
