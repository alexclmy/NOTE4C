import { NextResponse } from "next/server";
import {
  NO_REMOTE_WAKE_NOTE,
  batteryUnavailableReason,
  describeReachability,
  deviceSupportsPower,
  estimateNextWake,
} from "@/core/power";
import { guarded } from "@/server/auth/guard";
import { MANUAL_STATUS_RETRY_BUDGET_MS } from "@/server/device/client";
import { deviceContext } from "@/server/device/context";
import { describeDeviceFailure } from "@/server/device/failure";
import {
  readLastConfirmed,
  readingFromStatus,
  recordConfirmedReading,
} from "@/server/device/lastConfirmed";
import { readLastKnownDevice } from "@/server/device/lastKnown";
import { blockingPush, readPushes, resolveSha } from "@/server/device/ledger";
import { describeIntent } from "@/server/device/powerIntent";
import { readState } from "@/server/store/state";

export const dynamic = "force-dynamic";
export const revalidate = 0;

/**
 * Said on the response as well as asked for on the request.
 *
 * The browser already sends `cache: "no-store"` (src/ui/api.ts), but a reading
 * of a device is exactly the kind of answer an intermediary would think it was
 * being helpful by keeping, and "Check now" returning a cached sentence about
 * an earlier moment is the defect this whole route is being corrected for.
 */
const NO_STORE = { "cache-control": "no-store, must-revalidate" } as const;

/**
 * The device's own answer, plus the tower's honest reading of it. Nothing here
 * is inferred: an unreachable device produces reachable:false and a reason,
 * never a cached status pretending to be current.
 *
 * **This route does not change anything, and must not.** It reads the device,
 * reads the tower's state, and reports both. It used to also deliver a pending
 * power intent, which meant a GET could PATCH the device's configuration and
 * rewrite the state file — a mutation on a route with no CSRF check, triggered
 * by anything that could cause a browser to issue this GET. Delivery now
 * happens on `POST /api/device/power` (session + CSRF, like every other
 * mutation) and on the background scheduler. What is reported here is
 * `describeIntent`, which runs the same decision and writes nothing, so the
 * sentence a reader sees is the one the next reconcile will act on.
 *
 * `?force=1` marks the read a person asked for — the "Check now" button. Two
 * things follow from it, and neither is a mutation: the read is allowed a
 * longer budget of read-only retries (see `DeviceClient.status`, and the note
 * on MANUAL_STATUS_RETRY_BUDGET_MS for the twenty-second kernel hold-down it
 * has to outlast), and the answer is marked so a test and a reader can tell a
 * deliberate read from a mount.
 *
 * `?observe=0` asks the opposite question: *what do you already know*. No
 * socket is opened, the answer is local files only, and it comes back in under
 * a millisecond whatever the panel is doing. It is what the Device page paints
 * its first frame from, so that a device which takes ten seconds to time out
 * costs ten seconds of a *badge* rather than ten seconds of a blank page. The
 * payload carries `observed: false` and asserts nothing about reachability —
 * see the note on `src/server/device/lastKnown.ts`.
 */
export const GET = guarded({}, async ({ request }) => {
  const params = request.nextUrl.searchParams;

  // Before `deviceContext()`, which in mock mode starts a server and in real
  // mode reads a token off disk. Neither is expensive, but this path's whole
  // promise is that it touches nothing it does not have to.
  if (params.get("observe") === "0") {
    return NextResponse.json(readLastKnownDevice(), { headers: NO_STORE });
  }

  const { client, state, simulated, tokenConfigured } = await deviceContext();
  const manual = params.get("force") === "1";

  const blocking = blockingPush();
  const lastPush = readPushes()[0] ?? null;

  try {
    const status = await client.status(
      manual ? { retryBudgetMs: MANUAL_STATUS_RETRY_BUDGET_MS } : {},
    );
    const readingAt = new Date();
    // In memory only. This route is a read and stays one — see the note above
    // — and noting that the device answered, with what it said, is not a
    // mutation of anything durable. It is what lets the *next* failed read say
    // something true instead of guessing at "asleep".
    recordConfirmedReading(readingFromStatus(status, readingAt));
    const displayedMatch = resolveSha(status.displayed.sha256);
    const storedMatch = resolveSha(status.stored.sha256);

    // Described, not delivered. Reading the page is not a reason to write to
    // the device; see the note on this route above.
    const described = describeIntent(status, { reachable: true });
    const power = status.power ?? null;
    // The device is demonstrably up right now, but recording that is a write,
    // so the last-seen timestamp is left to the reconcile path. Until it has
    // run once the estimate falls back to the device's own armed timer, which
    // is the better source anyway.
    const lastSeen = readState().deviceLastSeenAt;
    const nextWake = estimateNextWake(power, lastSeen ? new Date(lastSeen) : null);

    return NextResponse.json(
      {
        observed: true,
        reachable: true,
        power,
        powerSupported: deviceSupportsPower(status.capabilities),
        powerIntent: described.intent,
        powerIntentDisposition: described.disposition.kind,
        powerIntentDetail: described.detail,
        deviceLastSeenAt: lastSeen,
        batteryUnavailableReason: power ? batteryUnavailableReason(power) : null,
        reachabilityNote: describeReachability(true, power),
        noRemoteWake: NO_REMOTE_WAKE_NOTE,
        nextWake: nextWake
          ? { at: nextWake.at.toISOString(), estimated: nextWake.estimated }
          : null,
        simulated,
        tokenConfigured,
        deviceMode: state.deviceMode,
        deviceAddress: simulated ? client.origin : state.deviceAddress,
        readAt: new Date().toISOString(),
        status,
        displayedMatch,
        storedMatch,
        storedEqualsDisplayed:
          status.stored.present &&
          status.stored.sha256 === status.displayed.sha256,
        blocking,
        lastPush,
        manual,
        // Returned on the success path too, so the interface never has to hold
        // two shapes: here it simply *is* the reading, taken just now.
        lastConfirmed: readingFromStatus(status, readingAt),
      },
      { headers: NO_STORE },
    );
  } catch (error) {
    // Usually not an outage. In automatic power saving this is the device's
    // normal state between refreshes, and there is nothing to record about it:
    // a sleeping device is the expected case, not an event.
    //
    // "Usually", because that assumption was wrong for the whole of one
    // outage. `failure` is what separates the two: `absent` is the sleeping
    // case and reads exactly as it did before, and anything else says that the
    // device was there, or that the tower never managed to ask. Still nothing
    // is written — this route is a read and must stay one.
    const failure = describeDeviceFailure(error);
    const described = describeIntent(null, { reachable: false });
    const lastSeen = readState().deviceLastSeenAt;
    // The one thing that can honestly be said about a device that did not
    // answer: what it said when it last did, and how long ago that was. This
    // is what the badge reasons from now; without it every failed read fell
    // through to "asleep", including for a panel that had just been woken.
    const lastConfirmed = readLastConfirmed();
    const nextWake = estimateNextWake(
      lastConfirmed?.power ?? null,
      lastSeen ? new Date(lastSeen) : null,
    );

    return NextResponse.json(
      {
        observed: true,
        reachable: false,
        // Null, and deliberately so: there is no current power block, and
        // rendering the stale one in this field would be the same lie in a new
        // place. The stale reading travels as `lastConfirmed`, with its age.
        power: null,
        powerSupported: null,
        powerIntent: described.intent,
        powerIntentDisposition: described.disposition.kind,
        powerIntentDetail: described.detail,
        batteryUnavailableReason: null,
        reachabilityNote: describeReachability(false, null),
        noRemoteWake: NO_REMOTE_WAKE_NOTE,
        deviceLastSeenAt: lastSeen,
        nextWake: nextWake
          ? { at: nextWake.at.toISOString(), estimated: nextWake.estimated }
          : null,
        simulated,
        tokenConfigured,
        deviceMode: state.deviceMode,
        deviceAddress: simulated ? null : state.deviceAddress,
        readAt: new Date().toISOString(),
        detail: failure.detail,
        failure,
        blocking,
        lastPush,
        manual,
        lastConfirmed,
      },
      { headers: NO_STORE },
    );
  }
});
