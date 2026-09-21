import { z } from "zod";

/**
 * Value shapes the server adapters produce and the modules render. Kept as
 * zod schemas so the boundary between adapter and renderer is validated in
 * both directions, and so fixtures in tests cannot drift from reality.
 */

export const WeatherSlotSchema = z.object({
  /** Local hour label, e.g. "07h". */
  time: z.string(),
  temp: z.number().int(),
  condition: z.string(),
});
export type WeatherSlot = z.infer<typeof WeatherSlotSchema>;

/** One day of the multi-day outlook: a weekday label, a condition, a range. */
export const DayForecastSchema = z.object({
  /** Short weekday label, e.g. "TUE". */
  label: z.string(),
  condition: z.string(),
  high: z.number().int(),
  low: z.number().int(),
});
export type DayForecast = z.infer<typeof DayForecastSchema>;

export const WeatherValueSchema = z.object({
  slots: z.array(WeatherSlotSchema).length(4),
  low: z.number().int(),
  high: z.number().int(),
  /** How many validated hourly entries backed this forecast. */
  hours: z.number().int(),
  unit: z.string(),
  locationLabel: z.string(),
  locationWarning: z.boolean(),
  /** Condition now, used by the octopus. */
  condition: z.string(),
  /**
   * The multi-day outlook, when the source provided one. Optional: the hourly
   * fallback source has no daily block, and a module that needs days renders an
   * explicit unavailable state rather than inventing them.
   */
  days: z.array(DayForecastSchema).optional(),
});
export type WeatherValue = z.infer<typeof WeatherValueSchema>;

export const CalendarEventSchema = z.object({
  title: z.string(),
  /** Pre-formatted local label, e.g. "12/09 08:30" or "12/09 · journée". */
  when: z.string(),
  start: z.string(),
  end: z.string(),
});
export type CalendarEvent = z.infer<typeof CalendarEventSchema>;

export const CalendarValueSchema = z.object({
  events: z.array(CalendarEventSchema),
});
export type CalendarValue = z.infer<typeof CalendarValueSchema>;

export const SensorValueSchema = z.object({
  label: z.string(),
  /** Already formatted for the panel, unit included. */
  value: z.string(),
});
export type SensorValue = z.infer<typeof SensorValueSchema>;

/**
 * What the browser and the renderer are allowed to know about Apple Reminders.
 *
 * Counts, and nothing else. The server-side adapter reads titles, due dates
 * and priorities because it has to sort and filter on them, and it keeps every
 * one of them on the server: no module in this build renders a reminder's
 * words, so sending them to a browser would be collecting private content for
 * no purpose. A later Reminders tile can widen this deliberately, with its own
 * decision recorded; it cannot happen by accident.
 */
/**
 * The list a Reminders-bound module starts on, before anybody chooses one.
 *
 * "Reminders" is the name of the default list macOS creates, so it is the one
 * guess most likely to be right and the least likely to be somebody else's.
 * This used to be one household's shared list, emoji and all, which meant a
 * fresh install of this software went looking for a list that exists on
 * exactly one Mac.
 *
 * It lives in core rather than beside the adapter because a module's schema
 * needs it and a module must not import anything that reaches for a child
 * process.
 */
export const DEFAULT_REMINDERS_LIST = "Reminders";

export const RemindersValueSchema = z.object({
  /** Incomplete reminders in the bound list. */
  openCount: z.number().int().nonnegative(),
  /** Of those, the ones whose due date has passed. */
  overdueCount: z.number().int().nonnegative(),
  /** The list the count came from, which is configuration, not content. */
  listName: z.string(),
});
export type RemindersValue = z.infer<typeof RemindersValueSchema>;

/**
 * Everything the renderer needs from the outside world for one dashboard.
 * Sensors are keyed by entity id because several tiles can bind to different
 * entities in the same dashboard.
 */
export interface DashboardSources {
  weather: import("./types").ModuleData<WeatherValue>;
  calendar: import("./types").ModuleData<CalendarValue>;
  sensors: Record<string, import("./types").ModuleData<SensorValue>>;
  reminders: import("./types").ModuleData<RemindersValue>;
}

export function emptySources(): DashboardSources {
  return {
    weather: { state: "unavailable" },
    calendar: { state: "unavailable" },
    sensors: {},
    reminders: { state: "unavailable" },
  };
}
