import { NextResponse } from "next/server";
import { z } from "zod";
import {
  INTERACTIVE_MINUTES,
  MAX_WAKE_INTERVAL_MIN,
  MIN_WAKE_INTERVAL_MIN,
  NO_REMOTE_WAKE_NOTE,
  POWER_MODES,
  deviceSupportsPower,
  estimateNextWake,
} from "@/core/power";
import { appendAudit } from "@/server/audit";
import { guarded, jsonError } from "@/server/auth/guard";
import { deviceContext } from "@/server/device/context";
import {
  clearIntent,
  noteDeviceSeen,
  readIntent,
  reconcileIntent,
  recordIntent,
  sleepIdempotencyKey,
  sleepNow,
} from "@/server/device/powerIntent";
import { invalidateOverviewSnapshot } from "@/server/overview/snapshot";
import { readState } from "@/server/store/state";

export const dynamic = "force-dynamic";

/**
 * The power controls.
 *
 * What this route will never do
 * -----------------------------
 * Claim to have woken a sleeping device. There is no such operation: deep
 * sleep powers the Wi-Fi radio down and nothing on the network can reach it.
 * Every response from here says plainly whether the change reached the device
 * or is being held until it next wakes, and the one immediate wake that exists
 * is the button on the device itself.
 *
 * That is why "set-mode" is two outcomes rather than one, and why the pending
 * one is not an error: it is the designed behaviour of a device that is asleep
 * fifty-nine minutes out of sixty.
 */

const BodySchema = z.discriminatedUnion("action", [
  z.object({
    action: z.literal("set-mode"),
    mode: z.enum(POWER_MODES),
    /** Required for `interactive`, ignored otherwise. One of the four. */
    minutes: z
      .number()
      .int()
      .refine((v) => (INTERACTIVE_MINUTES as readonly number[]).includes(v), {
        message: `minutes must be one of ${INTERACTIVE_MINUTES.join(", ")}`,
      })
      .optional(),
    /** Optional: change the auto-saver wake interval in the same request. */
    wakeIntervalMinutes: z
      .number()
      .int()
      .min(MIN_WAKE_INTERVAL_MIN)
      .max(MAX_WAKE_INTERVAL_MIN)
      .optional(),
  }),
  /** Withdraw a request the device has not picked up yet. */
  z.object({ action: z.literal("cancel-intent") }),
  /** Tell an awake device to go back to sleep now. Needs it to be awake. */
  z.object({
    action: z.literal("sleep-now"),
    /**
     * Optional caller-supplied idempotency key.
     *
     * A client that generates one per click gets exact deduplication of its
     * own retries. Without one the route derives a key from a sixty-second
     * bucket, which still collapses a double-click but cannot tell two
     * genuinely separate clicks a second apart from one. Either way the header
     * the firmware requires carries a key that means something, which a fresh
     * UUID per request did not.
     */
    idempotencyKey: z.string().uuid().optional(),
  }),
  /**
   * Try to deliver a pending intent now.
   *
   * The explicit, authenticated, CSRF-checked home for the thing
   * `GET /api/device/status` used to do on every read. A page that wants the
   * intent delivered asks for it; reading the page does not.
   */
  z.object({ action: z.literal("reconcile") }),
]);

export const POST = guarded(
  { mutating: true, schema: BodySchema, action: "device.power" },
  async ({ body }) => {
    // Every action on this route either writes to the device or changes what
    // the tower is holding for it, and both are things the Overview snapshot
    // has a cached answer about. Dropped up front for the same reason as on
    // the mode route: one unconditional line beats remembering four.
    invalidateOverviewSnapshot();

    if (body.action === "cancel-intent") {
      // Answers in the same shape as every other action on this route —
      // applied/pending/detail — because the page renders the response the
      // same way whatever was asked for. Returning only {intent, cancelled}
      // meant the client read `detail` as undefined and showed the user
      // nothing at all after they clicked "cancel this request".
      const existing = readIntent();
      if (existing === null) {
        return NextResponse.json({
          applied: false,
          pending: false,
          cancelled: false,
          intent: null,
          detail:
            "There was no pending request to withdraw. It had already been delivered, or cancelled somewhere else.",
        });
      }
      clearIntent();
      const detail = `Withdrew the pending request to set ${existing.mode}. The device never heard it, so nothing on the device changed.`;
      appendAudit({
        action: "device.power.intent.cancel",
        target: "tower",
        params: { mode: existing.mode },
        outcome: "ok",
        deviceConfirmed: null,
        detail: "Withdrew a power mode request the device had not picked up",
      });
      return NextResponse.json({
        applied: false,
        pending: false,
        cancelled: true,
        intent: null,
        detail,
      });
    }

    const { client, simulated, tokenConfigured } = await deviceContext();

    if (!simulated && !tokenConfigured) {
      return jsonError(
        400,
        "no_token",
        "Import or pair a device token before changing the power mode",
      );
    }

    // One read, shared by everything below. It is also how the tower finds out
    // whether the device is awake at all, which is the question the whole
    // route turns on.
    let status = null;
    let reachable = false;
    let unreachableDetail = "";
    try {
      status = await client.status();
      reachable = true;
      noteDeviceSeen(new Date(), status);
    } catch (error) {
      unreachableDetail =
        error instanceof Error ? error.message : "The device could not be reached";
    }

    if (body.action === "sleep-now") {
      // Not something that can be held as an intent: a sleeping device is
      // already asleep, and queuing "go to sleep" to be delivered at its next
      // wake would cancel the very refresh that wake exists to perform.
      if (!reachable) {
        return NextResponse.json({
          applied: false,
          alreadyAsleep: true,
          detail: `The device is not answering, which is what a sleeping device looks like. There is nothing to put to sleep. ${NO_REMOTE_WAKE_NOTE}`,
        });
      }
      if (!deviceSupportsPower(status?.capabilities)) {
        return jsonError(
          400,
          "unsupported",
          "This device's firmware does not advertise the hybrid power contract",
        );
      }
      try {
        // A key that means something. The caller's if it supplied one, else a
        // sixty-second bucket, so a double-click is one sleep rather than two.
        const key = body.idempotencyKey ?? sleepIdempotencyKey(new Date());
        const result = await sleepNow(client, key);
        // `applied` follows what the device actually did. A 200 that scheduled
        // nothing is the device declining — something outranks sleep — and
        // reporting that as applied was the route telling the user the device
        // had gone to sleep while it was demonstrably still awake.
        return NextResponse.json({
          applied: result.scheduled || result.replay,
          pending: false,
          scheduled: result.scheduled,
          replay: result.replay,
          detail: result.replay
            ? "That request had already been served; the device scheduled nothing new."
            : result.scheduled
              ? "The device acknowledged and is entering deep sleep. Only its own wake timer, or the button on the device, brings it back."
              : "The device accepted the request but scheduled no sleep. Something outranks it — a panel refresh in flight, the provisioning portal, or a running slideshow — so it stays awake for now.",
        });
      } catch (error) {
        return jsonError(
          502,
          "device_refused",
          error instanceof Error ? error.message : "The device refused the action",
        );
      }
    }

    if (body.action === "reconcile") {
      // Deliver whatever is already recorded. No new intent is created: this
      // is the "try again now" button, not a way to set a mode.
      const existing = readIntent();
      if (existing === null) {
        return NextResponse.json({
          applied: false,
          pending: false,
          intent: null,
          reachable,
          disposition: "none",
          detail: "There is no pending request to deliver.",
        });
      }
      const outcome = await reconcileIntent(client, status, { reachable });
      const state = readState();
      const wake = estimateNextWake(
        status?.power ?? null,
        state.deviceLastSeenAt ? new Date(state.deviceLastSeenAt) : null,
      );
      return NextResponse.json({
        applied: outcome.applied,
        pending: !outcome.applied && outcome.intent !== null,
        intent: outcome.intent,
        reachable,
        disposition: outcome.disposition.kind,
        detail: outcome.detail || unreachableDetail || NO_REMOTE_WAKE_NOTE,
        noRemoteWake: NO_REMOTE_WAKE_NOTE,
        nextWake: wake ? { at: wake.at.toISOString(), estimated: wake.estimated } : null,
        power: status?.power ?? null,
      });
    }

    // set-mode.
    //
    // The capability check comes FIRST, before anything is written down, and
    // the order is the whole of this fix. It used to run after `recordIntent`,
    // with `clearIntent()` on the refusal path — and `clearIntent` does not
    // undo an overwrite, it deletes whatever is there. So a tower holding a
    // pending "interactive" for a device that was asleep, asked to set a mode
    // while the device happened to be awake on a build with no hybrid
    // contract, answered 400 and threw the pending request away as a side
    // effect of refusing an unrelated one. A refusal must leave the tower
    // exactly as it found it.
    //
    // Only checked when the device actually answered: an unreachable device
    // has told us nothing about its capabilities, and refusing on a guess
    // would make the pending-intent machinery useless in precisely the case it
    // exists for.
    if (reachable && !deviceSupportsPower(status?.capabilities)) {
      return jsonError(
        400,
        "unsupported",
        "This device's firmware does not advertise the hybrid power contract, so the tower did not write anything and left any pending request alone",
      );
    }

    // Record first, deliver second: if the process dies between the two, the
    // request survives and is delivered on the next pass. Recording after a
    // successful write would lose exactly the requests that mattered.
    const intent = recordIntent({
      mode: body.mode,
      interactiveMinutes:
        body.mode === "interactive" ? (body.minutes ?? INTERACTIVE_MINUTES[1]) : null,
      wakeIntervalMinutes: body.wakeIntervalMinutes ?? null,
    });

    const result = await reconcileIntent(client, status, { reachable });

    const state = readState();
    const nextWake = estimateNextWake(
      status?.power ?? null,
      state.deviceLastSeenAt ? new Date(state.deviceLastSeenAt) : null,
    );

    appendAudit({
      action: "device.power.set_mode",
      target: reachable ? "device" : "tower",
      params: {
        mode: intent.mode,
        minutes: intent.interactiveMinutes,
        wake_interval: intent.wakeIntervalMinutes,
      },
      outcome: result.applied ? "ok" : "pending",
      deviceConfirmed: result.applied,
      detail: result.applied
        ? result.detail
        : `Held as a pending intent: ${result.detail || unreachableDetail}`,
    });

    return NextResponse.json({
      applied: result.applied,
      // The honest middle state, and the reason this route exists in this
      // shape. Not an error: a device asleep on schedule is working correctly.
      pending: !result.applied && result.intent !== null,
      intent: result.intent,
      reachable,
      disposition: result.disposition.kind,
      detail: result.applied
        ? result.detail
        : result.detail || unreachableDetail || NO_REMOTE_WAKE_NOTE,
      noRemoteWake: NO_REMOTE_WAKE_NOTE,
      nextWake: nextWake
        ? { at: nextWake.at.toISOString(), estimated: nextWake.estimated }
        : null,
      power: status?.power ?? null,
    });
  },
);
