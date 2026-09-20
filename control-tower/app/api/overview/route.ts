import { NextResponse } from "next/server";
import { deviceSupportsPower, estimateNextWake } from "@/core/power";
import { guarded } from "@/server/auth/guard";
import {
  readLastConfirmed,
  readingFromStatus,
} from "@/server/device/lastConfirmed";
import { blockingPush, readPushes, resolveSha } from "@/server/device/ledger";
import { deviceTokenIsSet } from "@/server/device/token";
import {
  OVERVIEW_SNAPSHOT_TTL_MS,
  readOverviewSnapshot,
} from "@/server/overview/snapshot";
import { sourceHealth } from "@/server/sources";
import { listDashboards, readRecord } from "@/server/store/dashboards";
import { readState } from "@/server/store/state";

export const dynamic = "force-dynamic";

/**
 * Everything the Overview page shows, assembled so the page makes a single
 * request.
 *
 * Two clocks, and they are different facts:
 *
 *  - `readAt` is when this response was assembled. Everything derived from the
 *    tower's own files — the ledger, the selected dashboard, the pending
 *    intent, the auto-refresh record — is that fresh, on every request,
 *    because those are local reads and because a page that showed a stale
 *    push result would be showing the wrong thing about an action the user
 *    just took.
 *  - `observedAt` is when the device and the sources were last actually read.
 *    That goes through `src/server/overview/snapshot.ts` and is at most
 *    `OVERVIEW_SNAPSHOT_TTL_MS` old, because the page polls once a minute and
 *    the alternative was a device round trip, a forecast fetch, a Home
 *    Assistant fetch and two spawned `remindctl` processes every sixty
 *    seconds, forever, for a tab somebody left open.
 *
 * The page renders the second next to the device readings rather than letting
 * the first stand in for it. `?force=1` skips the cache and waits for a fresh
 * read; that is what the "Read again" button and every post-action reload
 * send, so anything a person asked for is never answered from cache.
 *
 * **Without `force`, this route never waits on a device.** Not when the
 * snapshot is stale, and — since the loading regression this comment is being
 * extended for — not when there is no snapshot at all. A page load that had to
 * wait out two ten-second device timeouts before it could paint anything was
 * spending the reader's time on a block that is a tenth of the page; the other
 * nine tenths are local files and are assembled below on every request
 * regardless. `observed: false` says which is which.
 */
export const GET = guarded({}, async ({ request }) => {
  const now = new Date();
  const state = readState();

  const selected = state.selectedDashboardId
    ? readRecord(state.selectedDashboardId)
    : null;

  const force = new URL(request.url).searchParams.get("force") === "1";
  const read = await readOverviewSnapshot({
    doc: selected?.doc ?? null,
    now,
    force,
  });
  const snapshot = read.snapshot;
  const device = snapshot.device;

  // The same power facts the Device page reads, so the state strip says the
  // same word on both pages without a second read of the device. Derived, not
  // re-fetched: `device` above already carries the status this came from.
  const power = device.reachable ? (device.status.power ?? null) : null;
  const powerSupported = device.reachable
    ? deviceSupportsPower(device.status.capabilities)
    : null;
  // Fresh on every request, like everything else the route reads off disk. The
  // snapshot's own `simulated` is only true of a snapshot that was actually
  // built; the unread one has read nothing and cannot say.
  const simulated = state.deviceMode === "mock";
  // Likewise local, and likewise not taken from the snapshot: an unread one
  // has not built a client and cannot say whether a token is set, and
  // answering `false` for an install that has one would be a claim rather than
  // a gap. One 0600 file read, on a route that already does several.
  const tokenConfigured = simulated || deviceTokenIsSet();
  // The last answer the device gave, whenever that was. On the reachable path
  // this snapshot *is* that answer; otherwise it is whatever the tower last
  // recorded. Either way the badge on this page and the badge on Device reason
  // from the same evidence, which is the point of deriving both from
  // `deriveDeviceState`.
  const lastConfirmed =
    device.observed && device.reachable && snapshot.observedAt !== null
      ? readingFromStatus(device.status, new Date(snapshot.observedAt))
      : readLastConfirmed();
  const nextWake = estimateNextWake(
    power ?? lastConfirmed?.power ?? null,
    state.deviceLastSeenAt ? new Date(state.deviceLastSeenAt) : null,
  );

  const displayedSha = device.reachable ? device.status.displayed.sha256 : "";
  const storedSha = device.reachable ? device.status.stored.sha256 : "";

  // Resolved against the ledger on every request rather than cached with the
  // status: the shas come from the snapshot, but which dashboard version they
  // map to is a local file read, and a push that just landed must be named
  // correctly the moment the page asks.
  const lastVerified = readPushes().find(
    (record) => record.state === "verified_displayed",
  );

  return NextResponse.json({
    readAt: now.toISOString(),
    /** When the device and the sources below were actually read. */
    observedAt: snapshot.observedAt,
    /** True when that reading is older than the snapshot's own TTL. */
    observationStale: read.stale,
    snapshotTtlMs: OVERVIEW_SNAPSHOT_TTL_MS,
    simulated,
    tokenConfigured,
    deviceMode: state.deviceMode,
    deviceAddress:
      simulated && snapshot.deviceOrigin.length > 0
        ? snapshot.deviceOrigin
        : state.deviceAddress,
    device,
    /**
     * Whether the device and the sources in this response were read at all.
     *
     * False on the first request after a restart, while the read that will
     * replace them is in flight behind this response. The page paints its
     * structure from the local half either way and says, in the two blocks
     * that need the network, that they have not been read yet. See the note on
     * `unread` in src/server/overview/snapshot.ts.
     */
    observed: device.observed,
    /** True when a read was started behind this response. */
    refreshing: read.refreshing,
    displayedMatch: resolveSha(displayedSha),
    storedMatch: resolveSha(storedSha),
    storedEqualsDisplayed:
      device.reachable &&
      device.status.stored.present &&
      storedSha === displayedSha,
    selected: selected
      ? {
          id: selected.doc.id,
          title: selected.doc.title,
          latestVersion: selected.latestVersion,
          moduleCount: selected.doc.modules.length,
        }
      : null,
    autoRefresh: {
      intervalMinutes: selected?.doc.refreshIntervalMinutes ?? null,
      lastAt: state.lastAutoRefreshAt,
      lastDashboardId: state.lastAutoRefreshDashboardId,
      lastOutcome: state.lastAutoRefreshOutcome,
    },
    pending: snapshot.pending
      ? {
          sha256: snapshot.pending.sha256,
          semanticHash: snapshot.pending.semanticHash,
          wouldDedup: lastVerified?.first.semanticHash === snapshot.pending.semanticHash,
        }
      : null,
    sources: snapshot.sources ? sourceHealth(snapshot.sources) : [],
    composer: snapshot.composer,
    blocking: blockingPush(),
    lastPush: readPushes()[0] ?? null,
    power,
    powerSupported,
    powerIntent: state.powerIntent,
    nextWake: nextWake
      ? { at: nextWake.at.toISOString(), estimated: nextWake.estimated }
      : null,
    deviceLastSeenAt: state.deviceLastSeenAt,
    lastConfirmed,
    dashboardCount: listDashboards().length,
  });
});
