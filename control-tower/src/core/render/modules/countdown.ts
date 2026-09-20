import { z } from "zod";
import { BLACK, RED } from "@/core/palette";
import { ColourTokenSchema, colourFor } from "@/core/theme";
import {
  drawText,
  filled,
  reservedHeight,
  textElementSchema,
} from "../text";
import type { ModuleDefinition, PixelRect } from "../types";
import { PANEL_TIMEZONE } from "../time";
import { clearModule, inner, renderUnavailable, unavailableShape } from "./common";

/**
 * How long until a date the owner names.
 *
 * WHAT IT WILL NOT DO
 * -------------------
 * It will not guess a date. There is no "next Tuesday", no "in two weeks", no
 * parsing of the label: the target is an instant with an explicit offset, or
 * the module renders its configuration state. A countdown that inferred a date
 * would put a number on frozen ink that nobody chose.
 *
 * It will not render a zero for a missing target. "0 jours" is what a panel
 * says on the morning of the thing; it is not what a panel should say when
 * nobody has told it about anything.
 *
 * It does not tick. Everything below is a pure function of the `now` the
 * renderer was handed, so the preview, the packed bytes and the panel agree,
 * and nothing on this page refreshes per second for a display that refreshes
 * when it is pushed to.
 *
 * WHY DAYS ARE CALENDAR DAYS
 * --------------------------
 * "3 jours" on a kitchen panel means three more sleeps, not 72 hours. So days
 * are counted between calendar dates in the module's own timezone, which is
 * why the timezone is an option and not an assumption. Hours and minutes are
 * elapsed time, where that ambiguity does not arise.
 */

export const COUNTDOWN_UNITS = ["auto", "days", "hours", "minutes"] as const;
export type CountdownUnit = (typeof COUNTDOWN_UNITS)[number];

export const COUNTDOWN_AFTER = ["showPassed", "hide"] as const;

/** Is this a timezone this machine's Intl actually knows? */
export function isKnownTimeZone(value: string): boolean {
  try {
    new Intl.DateTimeFormat("en-CA", { timeZone: value });
    return true;
  } catch {
    return false;
  }
}

export const CountdownOptions = z.object({
  label: textElementSchema({
    text: "AVANT",
    maxLength: 40,
    style: { size: 13, weight: "bold" },
  }).default({}),
  /**
   * ISO 8601 WITH an offset. zod refuses a bare local time, which is the
   * point: a date with no offset is a date this module would have to guess a
   * timezone for.
   */
  targetAt: z.string().datetime({ offset: true }).nullable().default(null),
  /** Which calendar the days are counted in. */
  timeZone: z
    .string()
    .max(64)
    .refine(isKnownTimeZone, {
      message:
        "Not a timezone this machine knows. Use an IANA name such as Europe/Paris or UTC.",
    })
    .default(PANEL_TIMEZONE),
  unit: z.enum(COUNTDOWN_UNITS).default("auto"),
  /** {value} and {unit} are the renderer's; the sentence around them is not. */
  valueText: textElementSchema({
    text: "{value} {unit}",
    maxLength: 40,
    style: { family: "plexmono", size: 27, weight: "bold" },
  }).default({}),
  valueColour: ColourTokenSchema.default("inherit"),
  /** What the panel does once the date is behind it. Never a zero. */
  afterTarget: z.enum(COUNTDOWN_AFTER).default("showPassed"),
  passedText: textElementSchema({
    text: "C'est arrivé",
    maxLength: 40,
    style: { size: 18, weight: "bold" },
  }).default({}),
  ...unavailableShape({
    title: "Compte à rebours",
    note: "Aucune date cible définie",
  }),
});
export type CountdownOptions = z.infer<typeof CountdownOptions>;

/** The year, month and day a date falls on, in a given timezone. */
function civilDate(date: Date, timeZone: string): number {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).formatToParts(date);
  const pick = (type: Intl.DateTimeFormatPartTypes): number =>
    Number(parts.find((part) => part.type === type)?.value ?? "0");
  // Days since the epoch for that civil date, which makes the difference a
  // plain subtraction and keeps daylight saving out of it entirely.
  return Math.floor(
    Date.UTC(pick("year"), pick("month") - 1, pick("day")) / 86_400_000,
  );
}

/** Whole calendar days from one instant to another, in a timezone. */
export function calendarDaysBetween(
  from: Date,
  to: Date,
  timeZone: string,
): number {
  return civilDate(to, timeZone) - civilDate(from, timeZone);
}

export interface CountdownReading {
  state: "unset" | "invalid" | "passed" | "ahead";
  value?: number;
  unit?: "days" | "hours" | "minutes";
}

const UNIT_WORDS: Record<"days" | "hours" | "minutes", [string, string]> = {
  days: ["jour", "jours"],
  hours: ["heure", "heures"],
  minutes: ["minute", "minutes"],
};

/** French pluralisation for the three units. The machine writes this word. */
export function unitWord(
  unit: "days" | "hours" | "minutes",
  value: number,
): string {
  const [one, many] = UNIT_WORDS[unit];
  return Math.abs(value) <= 1 ? one : many;
}

/**
 * What this countdown reads at a given instant. Pure, and the whole of the
 * module's arithmetic, so the tests can pin it without a frame buffer.
 */
export function readCountdown(
  options: CountdownOptions,
  now: Date,
): CountdownReading {
  if (options.targetAt === null) return { state: "unset" };
  const target = new Date(options.targetAt);
  if (Number.isNaN(target.getTime())) return { state: "invalid" };
  if (target.getTime() <= now.getTime()) return { state: "passed" };

  const remainingMs = target.getTime() - now.getTime();
  const days = calendarDaysBetween(now, target, options.timeZone);
  const hours = Math.floor(remainingMs / 3_600_000);
  // Minutes round UP, so a target thirty seconds away reads "1 minute" and
  // never "0 minutes" while it is still ahead.
  const minutes = Math.ceil(remainingMs / 60_000);

  switch (options.unit) {
    case "days":
      return { state: "ahead", value: days, unit: "days" };
    case "hours":
      return { state: "ahead", value: hours, unit: "hours" };
    case "minutes":
      return { state: "ahead", value: minutes, unit: "minutes" };
    case "auto":
    default:
      if (days >= 1) return { state: "ahead", value: days, unit: "days" };
      if (hours >= 1) return { state: "ahead", value: hours, unit: "hours" };
      return { state: "ahead", value: minutes, unit: "minutes" };
  }
}

export const countdown: ModuleDefinition<CountdownOptions, never> = {
  type: "countdown",
  label: "Countdown",
  description:
    "How long until a date you name, with its timezone. Never guesses a date, never counts down live, and renders a configuration state rather than a zero when no target is set.",
  schema: CountdownOptions,
  defaultOptions: CountdownOptions.parse({}),
  defaultSpan: { w: 3, h: 2 },
  minSpan: { w: 2, h: 1 },
  maxSpan: { w: 8, h: 3 },
  sourceBinding: "none",

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    // ctx.now, not chromeNow: this number is the module's CONTENT. A panel
    // that should say "2 jours" tomorrow is a different panel, and the dedup
    // gate is entitled to know that.
    const reading = readCountdown(options, ctx.now);

    if (reading.state === "unset" || reading.state === "invalid") {
      // Not a zero and not a blank: an explicit statement that nobody has said
      // what to count to.
      renderUnavailable(fb, rect, options, reporter);
      return;
    }

    // "hide" means the whole tile goes, label included. A lone heading over
    // nothing would read as a countdown whose number failed to draw.
    if (reading.state === "passed" && options.afterTarget === "hide") return;

    const labelHeight = reservedHeight(options.label);
    if (labelHeight > 0) {
      drawText(fb, box, options.label, BLACK, "label", report);
    }
    const body: PixelRect = {
      x: box.x,
      y: box.y + labelHeight,
      w: box.w,
      h: Math.max(0, box.h - labelHeight),
    };

    if (reading.state === "passed") {
      drawText(fb, body, options.passedText, RED, "passedText", {
        wrap: true,
        ...report,
      });
      return;
    }

    const value = reading.value ?? 0;
    const unit = reading.unit ?? "days";
    drawText(
      fb,
      body,
      filled(options.valueText, { value, unit: unitWord(unit, value) }),
      colourFor(options.valueColour, BLACK),
      "valueText",
      { wrap: true, ...report },
    );
  },
};
