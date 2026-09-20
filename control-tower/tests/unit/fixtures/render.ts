import type { DashboardSources } from "@/core/render/data";
import type { RenderContext } from "@/core/render/types";

/**
 * Shared fixtures for the render tests.
 *
 * Generic on purpose. These strings end up in golden renders, in failure
 * messages and — through the same shapes the browser suite uses — in committed
 * screenshots. A fixture that names one household's routine is that
 * household's routine published as an example, so the calendar here is a
 * calendar anybody might have and the sensor is a room.
 *
 * The one thing deliberately NOT neutralised is the accented and multi-byte
 * text further down: the renderer has to survive it, and a fixture set made
 * entirely of ASCII would stop proving that.
 */

/** Fixed clock for every render fixture, so goldens never drift with the date. */
export const FIXTURE_NOW = new Date("2026-09-11T05:07:00.000Z");

export const FIXTURE_CTX: RenderContext = {
  now: FIXTURE_NOW,
  timeZone: "America/Toronto",
};

export const WEATHER_OK = {
  state: "ok",
  value: {
    slots: [
      { time: "01h", temp: 14, condition: "clear-night" },
      { time: "07h", temp: 12, condition: "rainy" },
      { time: "13h", temp: 21, condition: "partlycloudy" },
      { time: "19h", temp: 18, condition: "sunny" },
    ],
    low: 11,
    high: 22,
    hours: 24,
    unit: "°C",
    locationLabel: "City centre approx. · Open-Meteo",
    locationWarning: false,
    condition: "clear-night",
  },
} as const;

export const CALENDAR_OK = {
  state: "ok",
  value: {
    events: [
      {
        title: "Team standup",
        when: "11/09 09:00",
        start: "2026-09-11T13:00:00.000Z",
        end: "2026-09-11T14:00:00.000Z",
      },
      {
        title: "Parcel delivery, afternoon window",
        when: "12/09 · journée",
        start: "2026-09-12T04:00:00.000Z",
        end: "2026-09-13T04:00:00.000Z",
      },
    ],
  },
} as const;

export const SENSOR_OK = {
  state: "ok",
  value: { label: "Hallway", value: "19.4 °C" },
} as const;

export const UNAVAILABLE = { state: "unavailable" } as const;

/**
 * Reminders, as the browser-facing projection: counts only. Clearly synthetic,
 * and it has to be, because a fixture in git must never hold real household
 * content.
 */
export const REMINDERS_OK = {
  state: "ok",
  value: { openCount: 3, overdueCount: 1, listName: "FIXTURE LIST" },
  observedAt: "2026-09-11T05:06:30.000Z",
} as const;

export function fixtureSources(): DashboardSources {
  return {
    weather: WEATHER_OK as unknown as DashboardSources["weather"],
    calendar: CALENDAR_OK as unknown as DashboardSources["calendar"],
    sensors: {
      "sensor.example_temperature":
        SENSOR_OK as unknown as DashboardSources["sensors"][string],
    },
    reminders: REMINDERS_OK as unknown as DashboardSources["reminders"],
  };
}

export function noSources(): DashboardSources {
  return {
    weather: UNAVAILABLE,
    calendar: UNAVAILABLE,
    sensors: {},
    reminders: UNAVAILABLE,
  };
}
