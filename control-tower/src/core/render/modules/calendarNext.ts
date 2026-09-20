import { z } from "zod";
import { BLACK, RED } from "@/core/palette";
import type { CalendarValue } from "../data";
import {
  drawText,
  reservedHeight,
  textElementSchema,
  textStyleSchema,
} from "../text";
import type { ModuleDefinition } from "../types";
import {
  clearModule,
  inner,
  renderUnavailable,
  staleShape,
  unavailableShape,
} from "./common";

/**
 * The next few calendar events.
 *
 * Every fixed string this module used to draw — the heading, "Aucun
 * événement", "sur les 366 prochains jours", "Instantané périmé" — is now a
 * text element the owner can reword, restyle, or switch off. The event titles and
 * times stay data-bound: they are the calendar's to state, not the panel's.
 */
export const CalendarNextOptions = z.object({
  heading: textElementSchema({
    text: "À VENIR",
    maxLength: 40,
    style: { size: 15, weight: "bold" },
  }).default({}),
  maxEvents: z.number().int().min(1).max(4).default(2),
  /** The "11/09 09:00" line above each title. */
  whenStyle: textStyleSchema({ family: "plexmono", size: 13 }).default({}),
  /** The event title itself. */
  titleStyle: textStyleSchema({ size: 15 }).default({}),
  emptyTitle: textElementSchema({
    text: "Aucun événement",
    maxLength: 60,
    style: { size: 15 },
  }).default({}),
  emptyNote: textElementSchema({
    text: "sur les 366 prochains jours",
    maxLength: 60,
    style: { size: 13 },
  }).default({}),
  ...staleShape("Instantané périmé"),
  ...unavailableShape({
    title: "Calendrier indisponible",
    note: "Accès EventKit à vérifier",
  }),
});
export type CalendarNextOptions = z.infer<typeof CalendarNextOptions>;

export const calendarNext: ModuleDefinition<
  CalendarNextOptions,
  CalendarValue
> = {
  type: "calendarNext",
  label: "Calendar, next events",
  description:
    "Upcoming events from the configured calendar snapshot. Titles and times only: no locations, no attendees, no notes leave the Mac.",
  schema: CalendarNextOptions,
  defaultOptions: CalendarNextOptions.parse({}),
  defaultSpan: { w: 5, h: 2 },
  minSpan: { w: 3, h: 2 },
  maxSpan: { w: 8, h: 4 },
  sourceBinding: "calendar",

  migrateOptions(legacy) {
    return {
      ...(typeof legacy.heading === "string"
        ? { heading: { text: legacy.heading } }
        : {}),
      ...(typeof legacy.maxEvents === "number"
        ? { maxEvents: legacy.maxEvents }
        : {}),
    };
  },

  render(fb, rect, data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    if (data.state === "unavailable" || !data.value) {
      renderUnavailable(fb, rect, options, reporter);
      return;
    }

    let y = drawText(fb, box, options.heading, BLACK, "heading", report).nextY;
    const below = (): { x: number; y: number; w: number; h: number } => ({
      x: box.x,
      y,
      w: box.w,
      h: box.h - (y - box.y),
    });

    if (data.state === "stale") {
      y = drawText(fb, below(), options.staleLabel, RED, "staleLabel", report)
        .nextY;
    }

    const events = data.value.events.slice(0, options.maxEvents);

    if (events.length === 0) {
      y = drawText(fb, below(), options.emptyTitle, BLACK, "emptyTitle", report)
        .nextY;
      drawText(fb, below(), options.emptyNote, BLACK, "emptyNote", report);
      return;
    }

    // Each event needs a when line plus a title, and both heights follow
    // whatever typography the user chose, so a bigger title does not silently
    // collide with the event under it.
    //
    // Titles wrap to at most two lines and the row advances by however many
    // lines were actually drawn. Real calendar entries are long — a title of
    // forty-odd characters does not fit 250 px on one line at 15 px — and
    // wrapping uses the vertical room the tile already has instead of spending
    // an overflow warning on an ordinary appointment.
    const whenHeight = reservedHeight({
      text: "00/00 00:00",
      visible: true,
      style: options.whenStyle,
    });
    const titleHeight = reservedHeight({
      text: "Ag",
      visible: true,
      style: options.titleStyle,
    });

    for (const event of events) {
      // A row that could not show its when line and at least one title line is
      // not started: half an event on frozen ink reads as a different event.
      if (y + whenHeight + titleHeight > box.y + box.h + 6) {
        if (reporter) {
          reporter.overflow({
            role: "events",
            text: `${events.length} événements demandés`,
            kind: "height",
            neededPx: y + whenHeight + titleHeight - box.y,
            availablePx: box.h,
          });
        }
        break;
      }
      // The event's own words, passed through as data. Deliberately NOT run
      // through fillTemplate: a calendar entry called "Réunion {budget}" is a
      // string from the user's calendar, not a template for this module to
      // interpret.
      drawText(
        fb,
        { x: box.x, y, w: box.w, h: whenHeight },
        { text: event.when, visible: true, style: options.whenStyle },
        RED,
        "eventWhen",
        report,
      );
      const title = drawText(
        fb,
        {
          x: box.x,
          y: y + whenHeight,
          w: box.w,
          h: Math.max(0, box.y + box.h - (y + whenHeight)),
        },
        { text: event.title, visible: true, style: options.titleStyle },
        BLACK,
        "eventTitle",
        { wrap: true, maxLines: 2, ...report },
      );
      y = title.nextY + 4;
    }
  },
};
