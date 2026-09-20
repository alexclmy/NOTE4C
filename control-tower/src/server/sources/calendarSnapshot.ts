import fs from "node:fs";
import { z } from "zod";
import type { CalendarEvent, CalendarValue } from "@/core/render/data";
import { clean } from "@/core/font";
import { formatDayMonth, formatEventWhen } from "@/core/render/time";
import type { ModuleData } from "@/core/render/types";
import { calendarName, calendarSnapshotPath } from "@/server/config";
import { SourceError } from "./http";
import { notConfigured } from "./notConfigured";

/**
 * The calendar comes from a TCC-authorised EventKit worker that already runs
 * inside note4c-dashboard. The tower READS its snapshot and never writes to
 * that directory, never starts or stops the worker, and never touches
 * runtime/. Migrating the worker into the tower is a later phase.
 */
/** Exactly the string the worker writes. Anything else is not our snapshot. */
export const EXPECTED_SOURCE = "EventKit via authorized Terminal.app";
export const MAX_AGE_SECONDS = 180;

const SnapshotEventSchema = z
  .object({
    title: z.string(),
    start: z.string(),
    end: z.string(),
    all_day: z.boolean(),
    local_date: z.string(),
  })
  .strict();

const SnapshotSchema = z.object({
  schema: z.literal(1),
  source: z.literal(EXPECTED_SOURCE),
  /**
   * Whichever calendar the worker exported. The expected name is an optional
   * assertion applied after parsing (see validateSnapshot), not a structural
   * requirement: one installation's calendar name is not part of the file
   * format, and baking it into the schema rejected everybody else's snapshot
   * as malformed.
   */
  calendar: z.string(),
  state: z.literal("ok"),
  observed_at: z.string(),
  events: z.array(SnapshotEventSchema),
  tick: z.number().optional(),
  worker_pid: z.number().optional(),
});
export type CalendarSnapshot = z.infer<typeof SnapshotSchema>;

function parseInstant(value: string): Date {
  const date = new Date(value.replace(/Z$/, "+00:00"));
  if (Number.isNaN(date.getTime())) {
    throw new SourceError("Invalid calendar event time");
  }
  if (!/[+-]\d{2}:?\d{2}$/.test(value) && !/Z$/.test(value)) {
    throw new SourceError("Calendar event time is missing its offset");
  }
  return date;
}

export function validateSnapshot(raw: unknown, now: Date): CalendarSnapshot {
  const parsed = SnapshotSchema.safeParse(raw);
  if (!parsed.success) {
    throw new SourceError("Calendar snapshot scope, source or shape invalid");
  }
  const snapshot = parsed.data;

  // The optional assertion the schema used to make structurally. Set
  // NOTE4C_CALENDAR_NAME and a snapshot of a different calendar is refused;
  // leave it unset and any calendar the worker exports is accepted.
  const expected = calendarName();
  if (expected !== null && snapshot.calendar !== expected) {
    throw new SourceError(
      `Calendar snapshot is of "${snapshot.calendar}", not the configured "${expected}"`,
    );
  }

  const observedAt = new Date(snapshot.observed_at);
  if (Number.isNaN(observedAt.getTime())) {
    throw new SourceError("Calendar snapshot has no readable timestamp");
  }
  const ageSeconds = (now.getTime() - observedAt.getTime()) / 1000;
  if (ageSeconds < 0 || ageSeconds > MAX_AGE_SECONDS) {
    // Future-dated is as wrong as stale: both mean the worker and this process
    // disagree about the clock, and a calendar is not worth guessing about.
    throw new SourceError("Calendar snapshot stale or future-dated");
  }

  for (const event of snapshot.events) {
    const start = parseInstant(event.start);
    const end = parseInstant(event.end);
    if (end.getTime() < start.getTime()) {
      throw new SourceError("Calendar event ends before it starts");
    }
  }

  return snapshot;
}

/**
 * Titles and times only. Locations, attendees, notes and URLs never leave the
 * Mac, and the snapshot's strict field set means they cannot arrive here in
 * the first place.
 */
export function normalizeEvents(
  snapshot: CalendarSnapshot,
  now: Date,
  limit = 4,
): CalendarEvent[] {
  const unique = new Map<string, CalendarEvent>();

  for (const row of snapshot.events) {
    const start = parseInstant(row.start);
    const end = parseInstant(row.end);
    if (end.getTime() <= now.getTime()) continue;

    const title = clean(row.title || "Sans titre");
    const when = row.all_day
      ? `${formatDayMonth(new Date(`${row.local_date}T12:00:00Z`))} · journée`
      : formatEventWhen(start);

    unique.set(`${title}|${row.start}|${row.end}`, {
      title,
      when,
      start: start.toISOString(),
      end: end.toISOString(),
    });
  }

  return [...unique.values()]
    .sort((a, b) =>
      a.start === b.start ? a.title.localeCompare(b.title) : a.start.localeCompare(b.start),
    )
    .slice(0, limit);
}

export function readCalendarSnapshot(
  now: Date = new Date(),
  snapshotPath: string = calendarSnapshotPath(),
): ModuleData<CalendarValue> {
  if (snapshotPath.length === 0) {
    return {
      state: "unavailable",
      detail: notConfigured(
        "Set NOTE4C_CALENDAR_SNAPSHOT_PATH to the file an EventKit worker writes.",
      ),
    };
  }

  let raw: string;
  try {
    raw = fs.readFileSync(snapshotPath, "utf8");
  } catch {
    return {
      state: "unavailable",
      detail: "Calendar snapshot not present; the EventKit worker may be stopped",
    };
  }

  let payload: unknown;
  try {
    payload = JSON.parse(raw);
  } catch {
    return { state: "unavailable", detail: "Calendar snapshot is not valid JSON" };
  }

  try {
    const snapshot = validateSnapshot(payload, now);
    return {
      state: "ok",
      value: { events: normalizeEvents(snapshot, now) },
      observedAt: new Date(snapshot.observed_at).toISOString(),
    };
  } catch (error) {
    return {
      state: "unavailable",
      detail:
        error instanceof SourceError ? error.message : "Calendar unavailable",
    };
  }
}
