import type { ConfirmedReading } from "@/core/power";
import { deviceSupportsPower } from "@/core/power";
import { readState, updateState } from "../store/state";
import type { DeviceStatus } from "./client";

/**
 * The last answer the device actually gave.
 *
 * WHY THIS EXISTS
 * ---------------
 * A read that fails tells the tower nothing about the panel. Every honest
 * sentence about a device that is not answering therefore has to be built on
 * the last time it *did* answer — and until this file existed, the only thing
 * the tower kept was a timestamp. `deviceLastSeenAt` cannot say whether the
 * device was in automatic power saving, holding an interactive window open, or
 * set to stay awake, so `deriveDeviceState` had nothing to reason from and
 * fell through to "asleep" on every failed read. See the note at the top of
 * `deriveDeviceState` for what that cost.
 *
 * TWO PLACES, ON PURPOSE
 * ----------------------
 * The reading lives in this process's memory *and* in the tower's state file,
 * and the two are written by different callers for a reason that matters:
 *
 *  - **Memory** is written by every successful status read, including the ones
 *    behind `GET /api/device/status`. That route is a read and must stay one —
 *    it used to be able to PATCH the device and rewrite state.json, on a route
 *    with no CSRF check, and that is not being reintroduced. Noting in RAM that
 *    the device answered is not a mutation of anything durable: nothing on disk
 *    changes, nothing is sent anywhere, and a restart forgets it.
 *  - **Disk** is written only by paths that were already writing — the
 *    scheduler's window pass, the power route's reconcile, the queued-push
 *    delivery — through `noteDeviceSeen`, which has always recorded
 *    `deviceLastSeenAt` from exactly those places.
 *
 * A reader takes whichever is newer, so a fresh page load after a restart
 * still gets the last durable reading rather than nothing.
 *
 * ON SCHEMA VERSIONS
 * ------------------
 * The state field this adds is **optional and the document version does not
 * move**. A version bump would make a rollback fatal: this install's
 * control-tower.err.log is full of `schema_version 3 is newer than this build
 * understands (2)` from the last time a state document outran the binary
 * reading it. An optional field is invisible to an older build — zod strips
 * unknown keys — so the previous release can read, write and keep serving the
 * same file.
 */

export interface StoredReading extends ConfirmedReading {
  /** True when this came from the current process rather than from disk. */
  fromMemory?: boolean;
}

let inMemory: ConfirmedReading | null = null;

/** Turn a device answer into the reading the rest of the tower reasons from. */
export function readingFromStatus(
  status: DeviceStatus,
  now: Date = new Date(),
): ConfirmedReading {
  return {
    at: now.toISOString(),
    power: status.power ?? null,
    powerSupported: deviceSupportsPower(status.capabilities),
  };
}

/**
 * Note in memory that the device answered, and what it said.
 *
 * Safe to call from a GET: it writes nothing to disk and sends nothing
 * anywhere. Callers that are already mutating durable state should call
 * `noteDeviceSeen` instead, which does both.
 */
export function recordConfirmedReading(reading: ConfirmedReading): void {
  const known = inMemory;
  // Out-of-order responses are ordinary here — two pages and the scheduler can
  // all be reading at once — so an older answer never displaces a newer one.
  if (known !== null && Date.parse(known.at) > Date.parse(reading.at)) return;
  inMemory = reading;
}

/** Test seam: forget what this process observed. */
export function resetConfirmedReadingForTests(): void {
  inMemory = null;
}

/** Persist a reading to the tower state, and keep the memory copy in step. */
export function persistConfirmedReading(reading: ConfirmedReading): void {
  recordConfirmedReading(reading);
  updateState({ deviceLastSeenAt: reading.at, lastDeviceReading: reading });
}

/**
 * The newest reading this tower has, from either store, or null.
 *
 * Null is a real answer and is rendered as such: it means the device has never
 * told this tower anything, which is `uncertain`, not `asleep`.
 */
export function readLastConfirmed(): ConfirmedReading | null {
  const stored = readState().lastDeviceReading ?? null;
  if (stored === null) return inMemory;
  if (inMemory === null) return stored;
  return Date.parse(inMemory.at) >= Date.parse(stored.at) ? inMemory : stored;
}
