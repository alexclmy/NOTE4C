import { z } from "zod";
import { BLACK, RED } from "@/core/palette";
import { ColourTokenSchema, colourFor } from "@/core/theme";
import { drawText, reservedHeight, textElementSchema } from "../text";
import { DEFAULT_REMINDERS_LIST } from "../data";
import type {
  ModuleDefinition,
  PixelRect,
  RenderContext,
  SourceKind,
} from "../types";
import { PANEL_TIMEZONE } from "../time";
import { isKnownTimeZone } from "./countdown";
import { clearModule, inner, renderUnavailable, unavailableShape } from "./common";

/**
 * A message that is only on the panel when something is true.
 *
 * WHAT THIS IS NOT
 * ----------------
 * It is not an alarm and must never be sold as one. The NOTE4C is e-paper: it
 * shows whatever was last pushed to it, for as long as nobody pushes again. A
 * condition here is evaluated once, at render time, by whoever is pushing. If
 * the world changes afterwards the panel does not, and it has no way to tell
 * anyone. "Show the bin note on Tuesdays" is what this is for. Anything whose
 * absence would matter is not.
 *
 * THE CONDITION LANGUAGE
 * ----------------------
 * A small closed set of typed conditions, combined with all or any. No
 * expressions, no formulas, no strings that get interpreted: every condition
 * is an object the schema knows the shape of, every one of them can be
 * explained back to the owner in a sentence, and adding a kind means adding a case
 * here rather than teaching the panel to evaluate code.
 *
 * They are deterministic. Given the same `now` and the same source states, the
 * same conditions always come out the same way, which is what lets the preview
 * be the panel.
 */

/** Marker so the inspector gives the conditions a real editor. */
export const CONDITIONS_TAG = "conditions";

export const CONDITION_KINDS = [
  "dateRange",
  "daysOfWeek",
  "sourceState",
  "remindersCount",
] as const;
export type ConditionKind = (typeof CONDITION_KINDS)[number];

export const CONDITION_SOURCES = ["weather", "calendar", "reminders"] as const;
export const SOURCE_STATES = ["ok", "stale", "unavailable"] as const;

/** A plain YYYY-MM-DD, read in the condition's own timezone. */
const CivilDateSchema = z
  .string()
  .regex(/^\d{4}-\d{2}-\d{2}$/, "Use a plain date, YYYY-MM-DD")
  .nullable();

const TimeZoneSchema = z
  .string()
  .max(64)
  .refine(isKnownTimeZone, {
    message: "Not a timezone this machine knows. Use an IANA name.",
  })
  .default(PANEL_TIMEZONE);

export const ConditionSchema = z.discriminatedUnion("kind", [
  z.object({
    kind: z.literal("dateRange"),
    /** Inclusive. Either end may be open; both open is not configured. */
    from: CivilDateSchema.default(null),
    to: CivilDateSchema.default(null),
    timeZone: TimeZoneSchema,
  }),
  z.object({
    kind: z.literal("daysOfWeek"),
    /** 0 is Sunday, as JavaScript counts. An empty set is not configured. */
    days: z.array(z.number().int().min(0).max(6)).max(7).default([]),
    timeZone: TimeZoneSchema,
  }),
  z.object({
    kind: z.literal("sourceState"),
    source: z.enum(CONDITION_SOURCES),
    state: z.enum(SOURCE_STATES).default("ok"),
  }),
  z.object({
    kind: z.literal("remindersCount"),
    compare: z.enum(["atLeast", "atMost"]).default("atLeast"),
    value: z.number().int().min(0).max(99).default(1),
  }),
]);
export type Condition = z.infer<typeof ConditionSchema>;

export const DAY_LABELS = [
  "Sunday",
  "Monday",
  "Tuesday",
  "Wednesday",
  "Thursday",
  "Friday",
  "Saturday",
];

/** A sentence for this condition, for the inspector. English: tower chrome. */
export function explainCondition(condition: Condition): string {
  switch (condition.kind) {
    case "dateRange": {
      if (condition.from === null && condition.to === null) {
        return "On a date range — no dates set yet.";
      }
      if (condition.from !== null && condition.to !== null) {
        return `From ${condition.from} to ${condition.to} inclusive, in ${condition.timeZone}.`;
      }
      return condition.from !== null
        ? `From ${condition.from} onwards, in ${condition.timeZone}.`
        : `Up to and including ${condition.to}, in ${condition.timeZone}.`;
    }
    case "daysOfWeek": {
      if (condition.days.length === 0) return "On certain days — none chosen yet.";
      const names = [...condition.days]
        .sort((a, b) => a - b)
        .map((day) => DAY_LABELS[day] ?? String(day));
      return `On ${names.join(", ")}, in ${condition.timeZone}.`;
    }
    case "sourceState":
      return `When the ${condition.source} source is ${condition.state}.`;
    case "remindersCount":
      return condition.compare === "atLeast"
        ? `When the reminders list has ${condition.value} or more open.`
        : `When the reminders list has ${condition.value} or fewer open.`;
    default:
      return "Unknown condition.";
  }
}

/** Has this condition been given enough to decide anything? */
export function isConfigured(condition: Condition): boolean {
  if (condition.kind === "dateRange") {
    return condition.from !== null || condition.to !== null;
  }
  if (condition.kind === "daysOfWeek") return condition.days.length > 0;
  return true;
}

function civilDateString(date: Date, timeZone: string): string {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).formatToParts(date);
  const pick = (type: Intl.DateTimeFormatPartTypes): string =>
    parts.find((part) => part.type === type)?.value ?? "";
  return `${pick("year")}-${pick("month")}-${pick("day")}`;
}

function weekdayIn(date: Date, timeZone: string): number {
  const name = new Intl.DateTimeFormat("en-US", {
    timeZone,
    weekday: "short",
  }).format(date);
  const index = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].indexOf(
    name.slice(0, 3),
  );
  return index < 0 ? date.getUTCDay() : index;
}

export interface ConditionOutcome {
  value: boolean;
  /** Why, in words, for the designer. Never shown on the panel. */
  reason: string;
}

export function evaluateCondition(
  condition: Condition,
  ctx: RenderContext,
): ConditionOutcome {
  switch (condition.kind) {
    case "dateRange": {
      const today = civilDateString(ctx.now, condition.timeZone);
      const afterStart = condition.from === null || today >= condition.from;
      const beforeEnd = condition.to === null || today <= condition.to;
      return {
        value: afterStart && beforeEnd,
        reason: `Today is ${today} in ${condition.timeZone}`,
      };
    }
    case "daysOfWeek": {
      const day = weekdayIn(ctx.now, condition.timeZone);
      return {
        value: condition.days.includes(day),
        reason: `Today is ${DAY_LABELS[day] ?? day} in ${condition.timeZone}`,
      };
    }
    case "sourceState": {
      const sources = ctx.sources;
      if (!sources) {
        // Nothing was collected. That is not the same as the source being
        // down, and it is certainly not "true".
        return { value: false, reason: "No source data was collected" };
      }
      const actual =
        condition.source === "weather"
          ? sources.weather.state
          : condition.source === "calendar"
            ? sources.calendar.state
            : sources.reminders.state;
      return {
        value: actual === condition.state,
        reason: `The ${condition.source} source is ${actual}`,
      };
    }
    case "remindersCount": {
      const reminders = ctx.sources?.reminders;
      if (!reminders || reminders.state !== "ok" || !reminders.value) {
        return {
          value: false,
          reason: "The reminders list could not be read, so its count is unknown",
        };
      }
      const open = reminders.value.openCount;
      return {
        value:
          condition.compare === "atLeast"
            ? open >= condition.value
            : open <= condition.value,
        reason: `${open} reminders are open`,
      };
    }
    default:
      return { value: false, reason: "Unknown condition" };
  }
}

export interface ConditionsVerdict {
  /** False when nothing has been configured yet: not true, and not false. */
  configured: boolean;
  value: boolean;
  outcomes: Array<ConditionOutcome & { explain: string }>;
}

export function evaluateConditions(
  conditions: readonly Condition[],
  match: "all" | "any",
  ctx: RenderContext,
): ConditionsVerdict {
  const usable = conditions.filter(isConfigured);
  if (usable.length === 0) {
    return { configured: false, value: false, outcomes: [] };
  }
  const outcomes = usable.map((condition) => ({
    ...evaluateCondition(condition, ctx),
    explain: explainCondition(condition),
  }));
  const value =
    match === "all"
      ? outcomes.every((outcome) => outcome.value)
      : outcomes.some((outcome) => outcome.value);
  return { configured: true, value, outcomes };
}

export const ConditionalMessageOptions = z.object({
  body: textElementSchema({
    text: "",
    maxLength: 240,
    style: { size: 15 },
  }).default({}),
  match: z.enum(["all", "any"]).default("all"),
  conditions: z
    .array(ConditionSchema)
    .max(4)
    .default([])
    .describe(CONDITIONS_TAG),
  /** Which Reminders list a reminders condition asks about. */
  remindersList: z.string().min(1).max(80).default(DEFAULT_REMINDERS_LIST),
  /** What the tile does when the conditions do not hold. */
  whenFalse: z.enum(["hide", "showFallback"]).default("hide"),
  fallback: textElementSchema({
    text: "",
    maxLength: 240,
    visible: true,
    style: { size: 13 },
  }).default({}),
  /** An optional rule above the message, for a note that wants to stand out. */
  showRule: z.boolean().default(false),
  ruleColour: ColourTokenSchema.default("inherit"),
  ...unavailableShape({
    title: "Message conditionnel",
    note: "Condition à définir",
  }),
});
export type ConditionalMessageOptions = z.infer<
  typeof ConditionalMessageOptions
>;

export const conditionalMessage: ModuleDefinition<
  ConditionalMessageOptions,
  never
> = {
  type: "conditionalMessage",
  label: "Conditional message",
  description:
    "A note the panel shows only when a condition holds: a date range, certain days of the week, or the state of a source. Not an alarm: the panel only changes when it is pushed to, so nothing here can warn you about anything.",
  schema: ConditionalMessageOptions,
  defaultOptions: ConditionalMessageOptions.parse({}),
  defaultSpan: { w: 4, h: 1 },
  minSpan: { w: 2, h: 1 },
  maxSpan: { w: 8, h: 3 },
  sourceBinding: "none",

  extraSourceBindings(options): SourceKind[] {
    const needsReminders = options.conditions.some(
      (condition) =>
        condition.kind === "remindersCount" ||
        (condition.kind === "sourceState" && condition.source === "reminders"),
    );
    return needsReminders ? ["reminders"] : [];
  },

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    const verdict = evaluateConditions(options.conditions, options.match, ctx);

    if (!verdict.configured) {
      // No condition yet. A tile that showed its message anyway would be a
      // plain Message pretending to be conditional.
      renderUnavailable(fb, rect, options, reporter);
      return;
    }

    const element = verdict.value ? options.body : options.fallback;
    if (!verdict.value && options.whenFalse === "hide") return;

    let body: PixelRect = box;
    if (options.showRule) {
      fb.fillRect(box.x, box.y, box.w, 2, colourFor(options.ruleColour, BLACK));
      body = { x: box.x, y: box.y + 4, w: box.w, h: Math.max(0, box.h - 4) };
    }

    drawText(
      fb,
      body,
      element,
      verdict.value ? BLACK : RED,
      verdict.value ? "body" : "fallback",
      { wrap: true, ...report },
    );

    // A visible role with no words draws nothing, which for this module looks
    // exactly like "the condition was false". Say which it actually was.
    if (reporter?.note && reservedHeight(element) === 0) {
      reporter.note({
        role: verdict.value ? "body" : "fallback",
        kind: "empty-role",
        detail: verdict.value
          ? "The condition holds right now, but the message has no words, so the tile is blank."
          : "The condition does not hold and the fallback has no words, so the tile is blank.",
      });
    }
  },
};
