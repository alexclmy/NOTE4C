import type { DashboardDoc } from "@/core/model";
import type { DashboardSources } from "@/core/render/data";
import { moduleDefinition } from "@/core/render/modules";
import { readCalendarSnapshot } from "./calendarSnapshot";
import { readOpenMeteo } from "./openMeteo";
import { readSensors } from "./haSensor";
import { DEFAULT_REMINDERS_LIST, cachedRemindersValue } from "./appleReminders";
import { e2eSources } from "./e2eFixtures";
import { isNotConfigured } from "./notConfigured";

export * from "./http";
export * from "./openMeteo";
export * from "./calendarSnapshot";
export * from "./haSensor";
export * from "./composerFeed";
export * from "./appleReminders";
export * from "./notConfigured";

/** Which sources a dashboard actually binds to. Nothing else is fetched. */
export function sourceBindings(doc: DashboardDoc): {
  weather: boolean;
  calendar: boolean;
  sensors: Array<{ entityId: string; label: string }>;
  /** Null when no module asks for Reminders at all; otherwise the list name. */
  remindersList: string | null;
} {
  let weather = false;
  let calendar = false;
  let remindersList: string | null = null;
  const sensors: Array<{ entityId: string; label: string }> = [];

  for (const module of doc.modules) {
    let binding: string;
    let extra: string[] = [];
    let options: Record<string, unknown> = module.options;
    try {
      const definition = moduleDefinition(module.type);
      binding = definition.sourceBinding;
      const parsed = definition.schema.safeParse(module.options);
      if (parsed.success) options = parsed.data as Record<string, unknown>;
      if (definition.extraSourceBindings && parsed.success) {
        extra = definition.extraSourceBindings(parsed.data);
      }
    } catch {
      continue;
    }
    if (binding === "weather") weather = true;
    if (binding === "calendar") calendar = true;
    if (binding === "reminders" || extra.includes("reminders")) {
      // The list name is configuration on the module that asked for it. The
      // first module to ask decides, and the adapter resolves it by exact name
      // rather than by identifier.
      const configured = options.remindersList;
      if (remindersList === null) {
        remindersList =
          typeof configured === "string" && configured.trim().length > 0
            ? configured
            : DEFAULT_REMINDERS_LIST;
      }
    }
    if (binding === "haSensor") {
      const entityId = typeof options.entityId === "string" ? options.entityId : "";
      if (entityId.length > 0) {
        // The label is a text element now, so read its text. Falling back to
        // the entity id keeps this row identifiable when the label has been
        // emptied or hidden on the panel: this list is tower chrome, and
        // hiding a name from the e-paper is not a request to lose track of
        // which sensor is being read.
        const label = (options.label as { text?: unknown } | undefined)?.text;
        sensors.push({
          entityId,
          label:
            typeof label === "string" && label.trim().length > 0
              ? label
              : entityId,
        });
      }
    }
  }

  return { weather, calendar, sensors, remindersList };
}

const NOT_USED = {
  state: "unavailable" as const,
  detail: "Not used by this dashboard",
};

/**
 * Collect everything a dashboard needs. Adapters run in parallel and each one
 * absorbs its own failure into an explicit unavailable state, so one dead
 * source never blanks the whole panel.
 *
 * Reminders are read only when a module on this dashboard asks for them.
 * Private data is not collected speculatively so that a page can show a number
 * nobody requested.
 *
 * The Reminders read also goes through a five-minute cache
 * (`cachedRemindersValue`), so a page that polls cannot turn into two spawned
 * processes a minute against somebody's iCloud Reminders database. Nothing
 * else here is cached at this level: the weather adapter keeps its own cache,
 * and the calendar is a file read.
 */
export interface CollectOptions {
  /**
   * Bypass the Reminders cache.
   *
   * Set by the push route, and only there. A push paints frozen ink: it is
   * worth two subprocesses to be sure the count going onto the wall is the one
   * that is true right now. A page render is not, and the scheduler is not
   * either — its interval is at least fifteen minutes, comfortably longer than
   * the cache's own five, so its reading is fresh without asking.
   */
  force?: boolean;
}

export async function collectSources(
  doc: DashboardDoc,
  now: Date = new Date(),
  options: CollectOptions = {},
): Promise<DashboardSources> {
  // Browser QA runs against fixtures and reaches nothing: no forecast off the
  // internet, no calendar snapshot, no Home Assistant, and above all no
  // Reminders. Returns null unless NOTE4C_TOWER_E2E=1.
  const fixtures = e2eSources();
  if (fixtures) return fixtures;

  const bindings = sourceBindings(doc);

  const [weather, calendar, sensors, reminders] = await Promise.all([
    bindings.weather ? readOpenMeteo(now) : Promise.resolve(NOT_USED),
    bindings.calendar
      ? Promise.resolve(readCalendarSnapshot(now))
      : Promise.resolve(NOT_USED),
    bindings.sensors.length > 0
      ? readSensors(bindings.sensors, now)
      : Promise.resolve({}),
    bindings.remindersList !== null
      ? cachedRemindersValue({
          listName: bindings.remindersList,
          now,
          ...(options.force === true ? { force: true } : {}),
        })
      : Promise.resolve(NOT_USED),
  ]);

  return { weather, calendar, sensors, reminders };
}

export interface SourceHealthRow {
  key: string;
  label: string;
  state: "ok" | "stale" | "unavailable" | "not_configured";
  observedAt: string | null;
  detail: string | null;
}

export function sourceHealth(sources: DashboardSources): SourceHealthRow[] {
  const rows: SourceHealthRow[] = [
    {
      key: "weather",
      label: "Weather",
      // A source nobody configured is not a source that failed. The marker is
      // set by the adapter; see src/server/sources/notConfigured.ts.
      state: isNotConfigured(sources.weather.detail)
        ? "not_configured"
        : sources.weather.state,
      observedAt: sources.weather.observedAt ?? null,
      detail: sources.weather.detail ?? null,
    },
    {
      key: "calendar",
      label: "Calendar",
      state: isNotConfigured(sources.calendar.detail)
        ? "not_configured"
        : sources.calendar.state,
      observedAt: sources.calendar.observedAt ?? null,
      detail: sources.calendar.detail ?? null,
    },
  ];

  // Counts only, never a reminder's words. This row reaches Diagnostics in a
  // browser, and Reminders content has no business there.
  rows.push({
    key: "reminders",
    label: "Apple Reminders",
    state:
      sources.reminders.detail === NOT_USED.detail ||
      isNotConfigured(sources.reminders.detail)
        ? "not_configured"
        : sources.reminders.state,
    observedAt: sources.reminders.observedAt ?? null,
    detail:
      sources.reminders.state === "ok" && sources.reminders.value
        ? `${sources.reminders.value.openCount} open, ${sources.reminders.value.overdueCount} overdue`
        : (sources.reminders.detail ?? null),
  });

  const sensorEntries = Object.entries(sources.sensors);
  if (sensorEntries.length === 0) {
    rows.push({
      key: "sensors",
      label: "HA sensors",
      state: "not_configured",
      observedAt: null,
      detail: "No sensor tile on this dashboard",
    });
  } else {
    for (const [entityId, data] of sensorEntries) {
      rows.push({
        key: `sensor:${entityId}`,
        label: entityId,
        state: data.state,
        observedAt: data.observedAt ?? null,
        detail: data.detail ?? null,
      });
    }
  }

  return rows;
}
