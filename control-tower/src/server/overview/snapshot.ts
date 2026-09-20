/**
 * The expensive half of the Overview page, read at most once per interval.
 *
 * THE PROBLEM THIS SOLVES
 * -----------------------
 * `GET /api/overview` assembled everything the page shows, and the page polls
 * it every sixty seconds while the tab is open. Everything included:
 *
 *   - a `GET /api/v1/dashboard/status` at the device;
 *   - `collectSources()`, which is a forecast fetch, a Home Assistant fetch,
 *     a calendar file read and — before this — two `remindctl` processes,
 *     spawned synchronously, reaching into somebody's whole iCloud Reminders
 *     database;
 *   - `readComposerFeed()`, an HTTP call to a second program;
 *   - `prepareFrame()`, a full render of the dashboard to 30000 packed bytes.
 *
 * Once a minute, forever, for a laptop left on a page. None of it is cheap and
 * none of it needed doing that often: the device is asleep fifty-nine minutes
 * out of sixty and answers nothing, the forecast has a five-minute cache of
 * its own, and the count of open reminders changes a handful of times a day.
 *
 * WHAT REPLACES IT
 * ----------------
 * One snapshot, held in the process, with a TTL. A poll that finds it fresh
 * pays nothing. A poll that finds it stale gets the stale one *immediately*
 * and starts a refresh in the background, so the polling request itself never
 * waits on a device, a network or a subprocess.
 *
 * **Only an explicit `force` waits for a fresh one.** A first read with no
 * snapshot at all used to wait as well, and that was the loading regression:
 * it is every page load after a restart, and it put two serialised device
 * timeouts in front of the first card the reader could see. It now answers
 * with a snapshot that says, in the payload, that it has read nothing, and
 * starts the real read behind the response.
 *
 * WHAT IS NOT IN HERE, ON PURPOSE
 * -------------------------------
 * Everything the route can read from local files: the tower's own state, the
 * push ledger, the dashboard list, the pending intent. Those are fresh on
 * every single request. Caching them would mean a push whose result the page
 * then failed to show, and they cost a `readFileSync` of a small JSON file.
 *
 * HONESTY
 * -------
 * The snapshot carries `observedAt`, and the route reports it separately from
 * `readAt`. They are different facts — when this response was assembled, and
 * when the device was last actually asked — and the page renders the second
 * one next to the device readings rather than letting the first stand in for
 * it. A cache that let a page claim a two-second-old reading of a device it
 * had not contacted for four minutes would be the exact dishonesty the rest of
 * this product is built to avoid.
 */

import type { DashboardDoc } from "@/core/model";
import type { DashboardSources } from "@/core/render/data";
import { deviceContext } from "@/server/device/context";
import {
  readingFromStatus,
  recordConfirmedReading,
} from "@/server/device/lastConfirmed";
import { prepareFrame } from "@/server/device/pushPipeline";
import { collectSources } from "@/server/sources";
import { readComposerFeed, type ComposerFeed } from "@/server/sources/composerFeed";
import type { DeviceStatus } from "@/server/device/client";

/**
 * How long a snapshot is served before a refresh is started behind it.
 *
 * Five minutes, and it is chosen against what the page is looking at rather
 * than against what the tower could stand. The device sleeps between wakes, so
 * a minute-by-minute read of it mostly records "still not answering" a hundred
 * and twenty times an hour; the sources each change on the order of minutes at
 * best. Anything a person actually wants *now* has a "Read again" button that
 * forces, and every action the page takes forces on its way back.
 */
export const OVERVIEW_SNAPSHOT_TTL_MS = 5 * 60_000;

export type OverviewDevice =
  | { observed: true; reachable: true; status: DeviceStatus }
  | { observed: true; reachable: false; detail: string }
  /**
   * Nothing has been read. Not a failure — the absence of an attempt.
   *
   * This is what the page is handed on the first request after a restart,
   * while the read that will replace it is still in flight. It asserts nothing
   * about the panel; see the note on `DeviceStateInput.observed`.
   */
  | { observed: false; reachable: false; detail: string };

export interface OverviewSnapshot {
  /**
   * When the device and the sources were actually read, or null when they have
   * not been. Null is a real answer and the page renders it as one: a
   * freshness stamp on a reading that does not exist would be the dishonesty
   * this whole file's HONESTY note is about.
   */
  observedAt: string | null;
  /** Which dashboard the sources and the prepared frame are about, or null. */
  dashboardId: string | null;
  simulated: boolean;
  tokenConfigured: boolean;
  deviceOrigin: string;
  device: OverviewDevice;
  composer: ComposerFeed;
  sources: DashboardSources | null;
  pending: { sha256: string; semanticHash: string } | null;
}

interface CacheEntry {
  snapshot: OverviewSnapshot;
  fetchedAtMs: number;
}

let cache: CacheEntry | null = null;
let inFlight: Promise<OverviewSnapshot> | null = null;

/** Test seam, and the hook the test-only mock control route calls. */
export function invalidateOverviewSnapshot(): void {
  cache = null;
}

/**
 * Test seam: forget the cache, after letting any refresh in flight finish.
 *
 * Awaited rather than dropped, and that is not tidiness. A request that does
 * not force now starts a read *behind* its own response, so a test that made
 * one and did not wait leaves a socket open at the mock; the read lands during
 * whichever test runs next and is counted against it. Draining here is what
 * keeps "how many times did the tower open a socket" — the measurement this
 * whole file exists to make — a fact about one test rather than about two.
 */
export async function resetOverviewSnapshotForTests(): Promise<void> {
  await inFlight?.catch(() => undefined);
  cache = null;
  inFlight = null;
}

async function build(
  doc: DashboardDoc | null,
  now: Date,
): Promise<OverviewSnapshot> {
  const { client, simulated, tokenConfigured } = await deviceContext();

  const [device, composer, sources] = await Promise.all([
    client
      .status()
      .then((status) => {
        // In memory only, like the status route: a successful read here is the
        // freshest thing the tower knows about the panel, and the Device page's
        // next failed read has to be described against it rather than against a
        // guess. Nothing durable changes; see src/server/device/lastConfirmed.ts.
        recordConfirmedReading(readingFromStatus(status, now));
        return { observed: true as const, reachable: true as const, status };
      })
      .catch((error: unknown) => ({
        observed: true as const,
        reachable: false as const,
        detail:
          error instanceof Error ? error.message : "The device could not be reached",
      })),
    readComposerFeed(now),
    doc ? collectSources(doc, now) : Promise.resolve(null),
  ]);

  // What pushing the selection right now would produce, so the button can say
  // honestly whether it would change anything. Part of the snapshot because it
  // is a render of the sources above and is only true of them.
  let pending: OverviewSnapshot["pending"] = null;
  if (doc && sources) {
    const prepared = prepareFrame(doc, sources, now);
    pending = { sha256: prepared.sha256, semanticHash: prepared.semanticHash };
  }

  return {
    observedAt: now.toISOString(),
    dashboardId: doc?.id ?? null,
    simulated,
    tokenConfigured,
    deviceOrigin: client.origin,
    device,
    composer,
    sources,
    pending,
  };
}

export interface ReadSnapshotOptions {
  /** The selected dashboard, or null when nothing is selected. */
  doc: DashboardDoc | null;
  now?: Date;
  /** Skip the cache and wait for a fresh read. The "Read again" path. */
  force?: boolean;
  ttlMs?: number;
}

export interface SnapshotRead {
  snapshot: OverviewSnapshot;
  /** True when this response is older than the TTL. The page says so. */
  stale: boolean;
  /** True when a refresh was started behind this response. */
  refreshing: boolean;
}

/**
 * The snapshot, from cache where possible.
 *
 * Three outcomes, and the difference between the second and the third is the
 * whole point:
 *
 *  1. `force`: read now, and wait for it. The "Read again" button, and the
 *     only caller that is allowed to cost a device round trip.
 *  2. Fresh snapshot: return it, touch nothing.
 *  3. Stale snapshot: return it *now*, marked stale, and start one refresh
 *     behind it. The polling request does not wait.
 *  4. No snapshot at all: return one that says it has read nothing, and start
 *     the first read behind it. This used to be case 1, and making it wait was
 *     the whole of the loading regression — see the note on `unread`.
 *
 * A snapshot about a different dashboard than the one now selected is not
 * stale, it is wrong, so it is not served at all: that is case 4, with the
 * right read started behind it.
 */
export async function readOverviewSnapshot(
  options: ReadSnapshotOptions,
): Promise<SnapshotRead> {
  const now = options.now ?? new Date();
  const ttlMs = options.ttlMs ?? OVERVIEW_SNAPSHOT_TTL_MS;
  const wantedId = options.doc?.id ?? null;

  const usable =
    cache !== null && cache.snapshot.dashboardId === wantedId ? cache : null;

  if (options.force === true) {
    const snapshot = await refresh(options.doc, now);
    return { snapshot, stale: false, refreshing: false };
  }

  // No snapshot at all, and nobody asked to wait for one. This is every first
  // page load after a restart, and it used to be the case that blocked: the
  // request sat on `build()`, which sits on a device read, which on a panel
  // that accepts a socket and never answers is ten seconds — twice over,
  // because the shell's own read is ahead of it in the device mutex. Twenty
  // seconds of skeleton for a page that is mostly local files.
  //
  // So: answer now with a snapshot that says it has read nothing, and start
  // the real one behind it. The route's local half — the ledger, the
  // selection, the last confirmed reading — is assembled fresh either way, so
  // what the reader gets immediately is the whole page except the two blocks
  // that genuinely need the network, each of which says so.
  if (usable === null) {
    // `started` for the same reason the stale branch computes it: three
    // requests arriving together share one read, and only the first of them
    // can honestly say it caused one.
    const started = inFlight === null;
    void refresh(options.doc, now).catch(() => undefined);
    return { snapshot: unread(options.doc), stale: false, refreshing: started };
  }

  const age = now.getTime() - usable.fetchedAtMs;
  if (age >= 0 && age < ttlMs) {
    return { snapshot: usable.snapshot, stale: false, refreshing: false };
  }

  // Stale. Answer from the cache and refresh behind it, so the sixty-second
  // poll costs one file read and no sockets.
  const started = inFlight === null;
  void refresh(options.doc, now).catch(() => undefined);
  return { snapshot: usable.snapshot, stale: true, refreshing: started };
}

/**
 * The snapshot that has read nothing, assembled from nothing.
 *
 * Deliberately not cached: it is not a reading and must never be served in
 * place of one, or a tower whose device is permanently away would answer from
 * this forever and never try again.
 */
function unread(doc: DashboardDoc | null): OverviewSnapshot {
  return {
    observedAt: null,
    dashboardId: doc?.id ?? null,
    // Three fields this snapshot cannot answer, because answering them is what
    // `build()` does. The route does not read them off this object — it takes
    // all three from the tower's own state, fresh on every request — so these
    // are the empty values rather than plausible-looking guesses.
    simulated: false,
    tokenConfigured: false,
    deviceOrigin: "",
    device: {
      observed: false,
      reachable: false,
      detail: "The tower has not read the device yet.",
    },
    composer: {
      state: "unreadable",
      detail: "Not read yet.",
    },
    sources: null,
    pending: null,
  };
}

function refresh(doc: DashboardDoc | null, now: Date): Promise<OverviewSnapshot> {
  // One at a time. Without this, a stale snapshot plus two browser tabs plus a
  // slow device is three concurrent status reads at a panel that serves four
  // sockets and runs its own UI off the same server.
  if (inFlight !== null) return inFlight;
  inFlight = (async () => {
    try {
      const snapshot = await build(doc, now);
      cache = { snapshot, fetchedAtMs: now.getTime() };
      return snapshot;
    } finally {
      inFlight = null;
    }
  })();
  return inFlight;
}
