/**
 * Delivering a power mode to a device that is usually asleep.
 *
 * The rules live in src/core/power.ts and are host tested there. This is the
 * side that touches the durable state and opens a socket, and it exists so
 * that every caller — the power route, the status route, the refresh
 * scheduler — reconciles the same way and produces the same sentence.
 *
 * The shape of the problem
 * ------------------------
 * A device in automatic power saving is unreachable for fifty-nine minutes out
 * of sixty, and nothing on the network can change that: deep sleep powers the
 * Wi-Fi radio down. So "set the device to interactive" is not a request the
 * tower can always perform, and the two dishonest ways out are to fail the
 * click (which makes the feature useless) or to report success and hope
 * (which makes the UI a liar).
 *
 * The third way is to write the request down and deliver it when the device is
 * next actually there, which is what this file does, and to tell the user
 * exactly that in the meantime.
 */

import {
  deviceSupportsPower,
  intentToPatch,
  planIntent,
  sleepIdempotencyKey,
  type DevicePower,
  type IntentDisposition,
  type PowerIntent,
  type PowerMode,
} from "@/core/power";
import { appendAudit } from "@/server/audit";
import {
  DeviceRevisionConflict,
  type DeviceClient,
  type DeviceStatus,
} from "./client";
import { persistConfirmedReading, readingFromStatus } from "./lastConfirmed";
import { readState, updateState } from "@/server/store/state";

export interface RecordIntentOptions {
  mode: PowerMode;
  interactiveMinutes: number | null;
  wakeIntervalMinutes: number | null;
  now?: Date;
}

/** Write down what the user asked for. Never opens a socket. */
export function recordIntent(options: RecordIntentOptions): PowerIntent {
  const now = options.now ?? new Date();
  const intent: PowerIntent = {
    mode: options.mode,
    interactiveMinutes: options.interactiveMinutes,
    wakeIntervalMinutes: options.wakeIntervalMinutes,
    requestedAt: now.toISOString(),
    appliedAt: null,
    lastAttemptAt: null,
    lastAttemptError: null,
    attempts: 0,
  };
  updateState({ powerIntent: intent });
  return intent;
}

export function clearIntent(): void {
  updateState({ powerIntent: null });
}

export function readIntent(): PowerIntent | null {
  return readState().powerIntent;
}

/** Pull the power block out of a status read, or null if it is not there. */
export function powerFrom(status: DeviceStatus | null): DevicePower | null {
  return status?.power ?? null;
}

export interface ReconcileResult {
  disposition: IntentDisposition;
  /** The intent as it stands after this pass, or null once it is resolved. */
  intent: PowerIntent | null;
  /** True when this call actually wrote to the device. */
  applied: boolean;
  /** One sentence, safe to show the user. */
  detail: string;
}

/**
 * Say what *would* happen to the pending intent, without doing any of it.
 *
 * This is what a GET reads. It opens no socket, writes no state file and
 * touches no audit log: it runs the same `planIntent` the apply path runs and
 * reports the answer, so the sentence a reader sees is the one the next
 * reconcile will act on.
 *
 * Splitting this out is not tidiness. `GET /api/device/status` used to call
 * `reconcileIntent`, which meant a plain read of the status page could PATCH
 * the device's configuration and rewrite the tower's durable state — a
 * mutation on a route with no CSRF check, reachable from anything that could
 * make the browser issue a GET, and one that made "look at the page" and
 * "change the device" the same action.
 */
export function describeIntent(
  status: DeviceStatus | null,
  options: { reachable: boolean; now?: Date },
): ReconcileResult {
  const intent = readIntent();
  const disposition = planIntent({
    intent,
    power: powerFrom(status),
    reachable: options.reachable,
    supported: deviceSupportsPower(status?.capabilities),
    now: options.now ?? new Date(),
  });
  const detail =
    disposition.kind === "waiting" ||
    disposition.kind === "expired" ||
    disposition.kind === "backoff"
      ? disposition.reason
      : "";
  return { disposition, intent, applied: false, detail };
}

/**
 * Deliver a pending intent if the device is there, and say what happened.
 *
 * Callable on every status read. It is deliberately cheap and idempotent in
 * the common cases: no intent, or an intent whose device is asleep, both cost
 * nothing beyond the read the caller already did.
 *
 * @param status the status the caller already read. Passing it in rather than
 *        reading again keeps this to one round trip and keeps the reconcile
 *        consistent with the numbers the caller is about to render.
 */
export async function reconcileIntent(
  client: DeviceClient,
  status: DeviceStatus | null,
  options: { now?: Date; reachable: boolean } = { reachable: false },
): Promise<ReconcileResult> {
  const now = options.now ?? new Date();
  const intent = readIntent();
  const power = powerFrom(status);

  const disposition = planIntent({
    intent,
    power,
    reachable: options.reachable,
    supported: deviceSupportsPower(status?.capabilities),
    now,
  });

  switch (disposition.kind) {
    case "none":
      return { disposition, intent, applied: false, detail: "" };

    case "waiting": {
      // Nothing is written here, and that is the whole of it.
      //
      // This used to increment `attempts` and stamp `lastAttemptAt` on every
      // pass, which made both fields meaningless: the scheduler comes round
      // every thirty seconds whether or not the device is there, so an intent
      // waiting for a device that sleeps for an hour accumulated a hundred and
      // twenty "attempts" without a single packet being sent. The UI then
      // showed that count next to the request, which read as a device
      // stubbornly refusing a hundred and twenty times rather than a tower
      // patiently waiting once.
      //
      // `attempts` now counts attempts: passes where a write was actually
      // made. Waiting is not an attempt, it costs nothing, and it needs no
      // record — how long this has been pending is `requestedAt` subtracted
      // from now, which the UI already does.
      return { disposition, intent, applied: false, detail: disposition.reason };
    }

    case "backoff": {
      // Nothing is written, on either side. The device is here and the intent
      // still stands, but the last write failed and the tower is deliberately
      // not re-sending it yet. Counting this as an attempt would make the
      // backoff eat its own budget: the delay doubles per attempt, so a pass
      // that increments the counter without sending anything would drive the
      // intent to MAX_INTENT_ATTEMPTS purely by the scheduler ticking.
      return { disposition, intent, applied: false, detail: disposition.reason };
    }

    case "expired": {
      clearIntent();
      appendAudit({
        action: "device.power.intent.expired",
        target: "tower",
        params: { mode: intent?.mode ?? "unknown" },
        outcome: "refused",
        deviceConfirmed: false,
        detail: disposition.reason,
      });
      return { disposition, intent: null, applied: false, detail: disposition.reason };
    }

    case "satisfied": {
      clearIntent();
      const detail = `The device is already in ${intent?.mode ?? "that"} mode; nothing needed writing.`;
      appendAudit({
        action: "device.power.intent.satisfied",
        target: "device",
        params: { mode: intent?.mode ?? "unknown" },
        outcome: "ok",
        deviceConfirmed: true,
        detail,
      });
      return { disposition, intent: null, applied: false, detail };
    }

    case "apply":
      break;
  }

  // The device is here. This is the one place a socket is opened.
  if (intent === null || status === null) {
    return { disposition, intent, applied: false, detail: "" };
  }

  const revision = status.config_revision;
  if (typeof revision !== "number") {
    // No compare-and-swap token means no safe write: the firmware requires
    // expected_revision and refuses without one. Guessing zero would be a
    // blind overwrite of whatever somebody changed at the device.
    const detail =
      "The device did not report a configuration revision, so the tower has nothing to compare against and refused to write.";
    return { disposition, intent, applied: false, detail };
  }

  try {
    await client.patchConfig(revision, disposition.set);
    const applied: PowerIntent = {
      ...intent,
      appliedAt: now.toISOString(),
      lastAttemptAt: now.toISOString(),
      lastAttemptError: null,
      attempts: intent.attempts + 1,
    };
    // Cleared rather than kept as a tombstone: the state file is the tower's
    // list of things still to do, and a delivered intent is not one.
    clearIntent();
    const detail = `Applied ${intent.mode} to the device.`;
    appendAudit({
      action: "device.power.apply",
      target: "device",
      params: { ...disposition.set, expected_revision: revision },
      outcome: "ok",
      deviceConfirmed: true,
      detail,
    });
    return { disposition, intent: applied, applied: true, detail };
  } catch (error) {
    // A revision conflict means somebody changed the device between our read
    // and our write. The intent is deliberately kept: it is still what the
    // user wants, and the next pass re-reads and retries against the revision
    // that actually holds. What is never done is re-sending with the device's
    // current revision, which is the blind overwrite the CAS exists to stop.
    const detail =
      error instanceof DeviceRevisionConflict
        ? "The device configuration changed between the read and the write, so nothing was written. The tower will try again on the next read."
        : error instanceof Error
          ? error.message
          : "The device refused the write";
    const updated: PowerIntent = {
      ...intent,
      lastAttemptAt: now.toISOString(),
      lastAttemptError: detail.slice(0, 200),
      attempts: intent.attempts + 1,
    };
    updateState({ powerIntent: updated });
    appendAudit({
      action: "device.power.apply",
      target: "device",
      params: disposition.set,
      outcome: "refused",
      deviceConfirmed: false,
      detail,
    });
    return { disposition, intent: updated, applied: false, detail };
  }
}

/**
 * Note that the device was seen, and what it said while it was there.
 *
 * Only called on a successful read. A failed read tells us nothing about when
 * the device was last up, and moving the timestamp on failure would push the
 * estimate forward every time the device was asleep, which is most of the time.
 *
 * The status is optional so the callers that genuinely do not have one keep
 * working; every caller that does should pass it, because the timestamp alone
 * cannot say whether the device was sleeping as designed or holding a window
 * open — and guessing at that is what reported a freshly woken panel as
 * `Asleep`. See src/server/device/lastConfirmed.ts.
 */
export function noteDeviceSeen(
  now: Date = new Date(),
  status: DeviceStatus | null = null,
): void {
  if (status !== null) {
    persistConfirmedReading(readingFromStatus(status, now));
    return;
  }
  updateState({ deviceLastSeenAt: now.toISOString() });
}

/**
 * Ask an awake device to go back to sleep immediately.
 *
 * This is the only power operation that is an *action* rather than a setting,
 * because it is not a state the device sits in: it is a thing it does once.
 * It needs the device to be awake, for the obvious reason.
 *
 * @param idempotencyKey the key to send. Required, and deliberately not
 *        defaulted: this used to mint a fresh `randomUUID()` on every call,
 *        which meant the header was present and did nothing. The firmware
 *        deduplicates by that key, so two requests carrying two fresh keys are
 *        two distinct actions — a double-click, or a client retry after a
 *        timeout the device had in fact served, scheduled a second sleep. The
 *        caller knows which requests are the same request; this function does
 *        not, so it asks. See `sleepIdempotencyKey` in src/core/power.ts.
 */
export async function sleepNow(
  client: DeviceClient,
  idempotencyKey: string,
): Promise<{ scheduled: boolean; replay: boolean }> {
  const result = await client.runAction("sleep", idempotencyKey);

  // Reported as what the device actually said, not as what was asked for.
  //
  // `scheduled` is the device's own answer to "is a sleep now pending", and a
  // 200 with `scheduled: false` and `replay: false` means the request was
  // accepted and nothing was scheduled — the device declined, most likely
  // because something outranks sleep (a refresh in flight, the provisioning
  // portal, a slideshow). Recording that as a confirmed success, which is what
  // this did, put a line in the audit log saying the device went to sleep when
  // it demonstrably had not.
  const replayDetail =
    "Repeat of an already-served request; the device scheduled nothing new, which is the idempotency key working.";
  const scheduledDetail =
    "The device acknowledged and will enter deep sleep. Only the button, or its own wake timer, brings it back.";
  const declinedDetail =
    "The device accepted the request but scheduled no sleep. Something outranks it — a panel refresh in flight, the provisioning portal, or a running slideshow — and the device stays awake.";

  appendAudit({
    action: "device.power.sleep_now",
    target: "device",
    params: { replay: result.replay, scheduled: result.scheduled },
    outcome: result.scheduled || result.replay ? "ok" : "refused",
    deviceConfirmed: result.scheduled || result.replay,
    detail: result.replay
      ? replayDetail
      : result.scheduled
        ? scheduledDetail
        : declinedDetail,
  });
  return { scheduled: result.scheduled, replay: result.replay };
}

export { sleepIdempotencyKey };

export { intentToPatch };
