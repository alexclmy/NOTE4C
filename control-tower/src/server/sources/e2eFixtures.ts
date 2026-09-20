import type { DashboardSources } from "@/core/render/data";
import { DEFAULT_REMINDERS_LIST } from "@/core/render/data";
import type { ModuleData } from "@/core/render/types";
import type { CalendarValue } from "@/core/render/data";

/** The one gate. Only the Playwright web server sets this. */
export function isE2E(): boolean {
  return process.env.NOTE4C_TOWER_E2E === "1";
}

/**
 * Source data for browser QA, so the suite never leaves this machine.
 *
 * Refuses unless NOTE4C_TOWER_E2E=1, which only the Playwright web server
 * sets. It is the same gate the test-only mock device route uses, and it is
 * checked here rather than at the call site so there is exactly one place that
 * decides whether a run is a test run.
 *
 * WHY THIS EXISTS
 * ---------------
 * Without it a browser run reaches api.open-meteo.com for the weather, the
 * EventKit worker's snapshot for the calendar, Home Assistant for the sensors
 * and, once a dashboard carries a reminders condition, `remindctl` for
 * somebody's actual Reminders list. That is four real dependencies for a suite
 * whose subject is the user interface, and it made the typography specs flaky
 * in exactly the way a network dependency makes things flaky: the desktop
 * project rendered a live forecast and the mobile project, five minutes later,
 * rendered the unavailable state, so a heading the first one could see the
 * second one could not.
 *
 * The values are obviously invented and the adapters themselves are covered by
 * the unit suite, which drives each one with its own fixtures.
 *
 * They are also deliberately GENERIC. These fixtures are what appears in the
 * screenshots in docs/images/, which are committed and published, and in every
 * Playwright trace and failure artefact. Anything recognisable here — a
 * household's routine, a place, a language nobody else in the project reads —
 * is a detail about one person's life shipped in a public repository under the
 * heading "example". So: plain English, errands anyone might have, a sensor
 * with a room name and nothing else.
 */
export function e2eSources(): DashboardSources | null {
  if (!isE2E()) return null;

  const observedAt = "2026-09-11T05:00:00.000Z";
  return {
    weather: {
      state: "ok",
      observedAt,
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
        locationLabel: "Fixture · browser QA",
        locationWarning: false,
        condition: "clear-night",
      },
    },
    calendar: {
      state: "ok",
      observedAt,
      value: {
        events: [
          {
            title: "Team standup",
            when: "11/09 09:00",
            start: "2026-09-11T13:00:00.000Z",
            end: "2026-09-11T14:00:00.000Z",
          },
          {
            title: "Parcel delivery",
            when: "12/09 · journée",
            start: "2026-09-12T04:00:00.000Z",
            end: "2026-09-13T04:00:00.000Z",
          },
        ],
      },
    },
    sensors: {
      "sensor.example_temperature": {
        state: "ok",
        observedAt,
        value: { label: "Hallway", value: "19.4 °C" },
      },
    },
    // Counts only, exactly as the real adapter projects them, and invented.
    reminders: {
      state: "ok",
      observedAt,
      value: { openCount: 3, overdueCount: 1, listName: DEFAULT_REMINDERS_LIST },
    },
  };
}

/**
 * The calendar, for the Diagnostics page, which reads it directly rather than
 * through collectSources. Same gate, same reason: a browser run must not go
 * looking for the EventKit worker's snapshot on this machine.
 */
export function e2eCalendar(): ModuleData<CalendarValue> | null {
  const sources = e2eSources();
  return sources ? sources.calendar : null;
}
