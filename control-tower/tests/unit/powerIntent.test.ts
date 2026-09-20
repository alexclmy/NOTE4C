/**
 * Delivering a power mode to a device that is usually asleep.
 *
 * Driven through the real DeviceClient against the faithful mock, with the
 * mock's `asleep` flag reproducing the state that matters most: a device whose
 * radio is off, which accepts no connection and answers nothing.
 *
 * The behaviour under test, stated once:
 *
 *   A request aimed at a sleeping device is recorded, reported as pending with
 *   an honest sentence, and delivered the next time the device is actually
 *   there. It is never reported as applied, never retried at a device that
 *   cannot answer, and never accompanied by an offer to wake it remotely.
 */

import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { MockDevice } from "@mock/server";
import { DeviceClient } from "@/server/device/client";
import {
  clearIntent,
  describeIntent,
  readIntent,
  reconcileIntent,
  recordIntent,
  sleepIdempotencyKey,
  sleepNow,
} from "@/server/device/powerIntent";
import {
  INTENT_RETRY_BASE_MS,
  INTENT_RETRY_MAX_MS,
  MAX_INTENT_ATTEMPTS,
} from "@/core/power";
import { readAudit } from "@/server/audit";
import { readState } from "@/server/store/state";
import { useTempDataRoot } from "./helpers/tempRoot";

const TOKEN = "e".repeat(64);

let temp: ReturnType<typeof useTempDataRoot>;
let device: MockDevice;
let client: DeviceClient;

beforeEach(async () => {
  temp = useTempDataRoot();
  device = new MockDevice({ token: TOKEN, panelDelayMs: 10 });
  const origin = await device.listen(0);
  client = new DeviceClient({
    mode: "mock",
    address: "192.168.7.7",
    mockOrigin: origin,
    token: TOKEN,
    // Short, so the asleep tests do not wait ten seconds for a timeout.
    timeoutMs: 500,
  });
});

afterEach(async () => {
  await device.close();
  temp.dispose();
});

/** Read the device the way the status route does, tolerating an asleep device. */
async function readStatus(): Promise<{ status: Awaited<ReturnType<DeviceClient["status"]>> | null; reachable: boolean }> {
  try {
    return { status: await client.status(), reachable: true };
  } catch {
    return { status: null, reachable: false };
  }
}

describe("recording an intent", () => {
  it("survives in durable state without touching the device", () => {
    const before = device.requestLog.length;
    recordIntent({ mode: "interactive", interactiveMinutes: 30, wakeIntervalMinutes: null });
    // Recording is a local write. A click that could only be recorded while
    // the device happened to be awake would be useless for this product.
    expect(device.requestLog.length).toBe(before);
    expect(readState().powerIntent).toMatchObject({
      mode: "interactive",
      interactiveMinutes: 30,
      appliedAt: null,
      attempts: 0,
    });
  });
});

describe("a device that is awake", () => {
  it("applies the intent immediately and clears it", async () => {
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    const { status, reachable } = await readStatus();
    const result = await reconcileIntent(client, status, { reachable });

    expect(result.applied).toBe(true);
    expect(result.disposition.kind).toBe("apply");
    expect(device.config.power.mode).toBe("always_on");
    // Delivered work is not still-to-do work.
    expect(readIntent()).toBeNull();
  });

  it("opens a live interactive window rather than persisting the mode", async () => {
    recordIntent({ mode: "interactive", interactiveMinutes: 30, wakeIntervalMinutes: null });
    const { status, reachable } = await readStatus();
    await reconcileIntent(client, status, { reachable });

    const after = await client.status();
    expect(after.power?.mode).toBe("interactive");
    expect(after.power?.interactive_remaining_s).toBeGreaterThan(1_700);
    // The *base* mode stays auto_saver: a window that came back from storage
    // after a reboot would be a window nobody opened.
    expect(device.config.power.mode).toBe("auto_saver");
  });

  it("writes the wake interval when one was chosen", async () => {
    recordIntent({ mode: "auto_saver", interactiveMinutes: null, wakeIntervalMinutes: 120 });
    const { status, reachable } = await readStatus();
    await reconcileIntent(client, status, { reachable });
    expect(device.config.power.wake_interval_min).toBe(120);
  });

  it("clears an intent the device already satisfies without writing", async () => {
    recordIntent({ mode: "auto_saver", interactiveMinutes: null, wakeIntervalMinutes: null });
    const { status, reachable } = await readStatus();
    const revisionBefore = device.configRevision;
    const result = await reconcileIntent(client, status, { reachable });

    expect(result.disposition.kind).toBe("satisfied");
    expect(result.applied).toBe(false);
    // Nothing was written, so the compare-and-swap counter did not move.
    expect(device.configRevision).toBe(revisionBefore);
    expect(readIntent()).toBeNull();
  });
});

describe("a device that is asleep", () => {
  beforeEach(() => {
    device.asleep = true;
  });

  /** The headline case. */
  it("holds the intent, reports pending, and never claims to have applied it", async () => {
    recordIntent({ mode: "interactive", interactiveMinutes: 15, wakeIntervalMinutes: null });
    const { status, reachable } = await readStatus();
    expect(reachable).toBe(false);

    const result = await reconcileIntent(client, status, { reachable });
    expect(result.applied).toBe(false);
    expect(result.disposition.kind).toBe("waiting");
    expect(readIntent()).not.toBeNull();
    expect(readIntent()?.appliedAt).toBeNull();
  });

  it("says the device cannot be woken from here, and names the button", async () => {
    recordIntent({ mode: "interactive", interactiveMinutes: 15, wakeIntervalMinutes: null });
    const { status, reachable } = await readStatus();
    const result = await reconcileIntent(client, status, { reachable });

    expect(result.detail).toMatch(/nothing on the network can wake it/i);
    expect(result.detail).toMatch(/button/i);
  });

  it("survives repeated passes without opening a socket to nowhere", async () => {
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    for (let i = 0; i < 3; i += 1) {
      const { status, reachable } = await readStatus();
      await reconcileIntent(client, status, { reachable });
    }
    // The intent survives every pass. What it does *not* do is accumulate an
    // attempt per pass: this assertion used to read `toBe(3)`, which encoded
    // the bug — see "waiting is not attempting" below for why that count was
    // both meaningless and actively misleading. How long the request has been
    // waiting is requestedAt subtracted from now, which needs no counter.
    expect(readIntent()?.attempts).toBe(0);
    expect(readIntent()?.appliedAt).toBeNull();
  });

  /**
   * The whole promise of the feature, end to end: the request outlives the
   * sleep and lands on the next wake.
   */
  it("delivers the held intent the moment the device comes back", async () => {
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });

    const asleep = await readStatus();
    const held = await reconcileIntent(client, asleep.status, { reachable: asleep.reachable });
    expect(held.applied).toBe(false);
    expect(device.config.power.mode).toBe("auto_saver");

    // The device wakes, the way only its timer or its button can make it.
    device.asleep = false;

    const awake = await readStatus();
    const delivered = await reconcileIntent(client, awake.status, { reachable: awake.reachable });
    expect(delivered.applied).toBe(true);
    expect(device.config.power.mode).toBe("always_on");
    expect(readIntent()).toBeNull();
  });

  it("refuses to pretend sleep-now did anything to an already sleeping device", async () => {
    // There is nothing to put to sleep, and nothing that could carry the
    // request there.
    await expect(sleepNow(client, sleepIdempotencyKey(new Date()))).rejects.toThrow();
    expect(device.performedActions).toHaveLength(0);
  });
});

describe("a device that changed under us", () => {
  it("keeps the intent after a revision conflict rather than overwriting blindly", async () => {
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    const { status } = await readStatus();
    // Somebody pressed a button on the device between the read and the write.
    device.bumpRevisionLocally((config) => {
      config.gallery.slide_min = 30;
    });

    const result = await reconcileIntent(client, status, { reachable: true });
    expect(result.applied).toBe(false);
    // Nothing was written: the compare-and-swap did its job.
    expect(device.config.power.mode).toBe("auto_saver");
    // And the intent is still there, because it is still what the user wants.
    // What must never happen is a re-send using the device's current revision,
    // which is exactly the blind overwrite the CAS exists to prevent.
    expect(readIntent()).not.toBeNull();
    expect(readIntent()?.lastAttemptError).toMatch(/changed between the read and the write/i);

    // The very next pass does NOT immediately re-send. One failed write buys
    // a backoff, because the scheduler comes round every thirty seconds and
    // re-sending on each tick is a hundred and twenty writes an hour at a
    // device that has already refused once.
    const immediateStatus = await readStatus();
    const immediate = await reconcileIntent(client, immediateStatus.status, {
      reachable: true,
    });
    expect(immediate.applied).toBe(false);
    expect(immediate.disposition.kind).toBe("backoff");
    expect(device.config.power.mode).toBe("auto_saver");
    // And the backoff pass is not itself counted as an attempt: nothing was
    // sent, so nothing should be charged against the retry budget.
    expect(readIntent()?.attempts).toBe(1);

    // Once the delay has run out, the next pass reads the new revision and
    // succeeds.
    const retryStatus = await readStatus();
    const retry = await reconcileIntent(client, retryStatus.status, {
      reachable: true,
      now: new Date(Date.now() + INTENT_RETRY_BASE_MS + 1_000),
    });
    expect(retry.applied).toBe(true);
    expect(device.config.power.mode).toBe("always_on");
  });

  it("gives up on an intent the device keeps refusing, instead of forever", async () => {
    // Eight real refusals over about ninety minutes. A ninth is not going to
    // get a different answer, and a PENDING badge that never resolves is worse
    // than being told plainly that the tower stopped.
    recordIntent({
      mode: "always_on",
      interactiveMinutes: null,
      wakeIntervalMinutes: null,
    });
    const { status } = await readStatus();

    let last = null as Awaited<ReturnType<typeof reconcileIntent>> | null;
    let now = Date.now();
    for (let i = 0; i < MAX_INTENT_ATTEMPTS + 1; i += 1) {
      // A device that refuses every write: bump the revision under us each
      // time, so the compare-and-swap fails on every attempt.
      device.bumpRevisionLocally((config) => {
        config.gallery.slide_min = 30 + i;
      });
      last = await reconcileIntent(client, status, {
        reachable: true,
        now: new Date(now),
      });
      if (last.disposition.kind === "expired") break;
      // Step past whatever backoff that attempt earned.
      now += INTENT_RETRY_MAX_MS + 1_000;
    }

    expect(last?.disposition.kind).toBe("expired");
    expect(readIntent()).toBeNull();
    expect(device.config.power.mode).toBe("auto_saver");
  });
});

describe("a device whose firmware has no power contract", () => {
  it("drops the intent rather than sending a field the device would refuse", async () => {
    await device.close();
    device = new MockDevice({
      token: TOKEN,
      panelDelayMs: 10,
      capabilities: ["dashboard.frame.v1", "dashboard.refresh.v1", "config.v2"],
    });
    const origin = await device.listen(0);
    client = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: origin,
      token: TOKEN,
      timeoutMs: 500,
    });

    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    const { status, reachable } = await readStatus();
    const result = await reconcileIntent(client, status, { reachable });

    expect(result.disposition.kind).toBe("expired");
    expect(result.detail).toMatch(/does not advertise/i);
    expect(readIntent()).toBeNull();
  });
});

describe("cancelling", () => {
  it("withdraws a request the device never picked up", async () => {
    device.asleep = true;
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    expect(readIntent()).not.toBeNull();
    clearIntent();
    expect(readIntent()).toBeNull();

    // And it stays withdrawn when the device comes back.
    device.asleep = false;
    const { status, reachable } = await readStatus();
    const result = await reconcileIntent(client, status, { reachable });
    expect(result.disposition.kind).toBe("none");
    expect(device.config.power.mode).toBe("auto_saver");
  });
});

describe("sleep now", () => {
  it("asks an awake device to sleep, with an idempotency key", async () => {
    const result = await sleepNow(client, sleepIdempotencyKey(new Date()));
    expect(result.scheduled).toBe(true);
    expect(device.performedActions.map((a) => a.action)).toEqual(["sleep"]);
  });

  /**
   * The idempotency key has to *be* idempotent.
   *
   * It used to be a fresh randomUUID() per call, which satisfies the
   * firmware's requirement that the header be present while defeating the
   * thing the header is for: two requests carrying two fresh keys are two
   * distinct actions, so a double-click or a retry after a timeout the device
   * had in fact served scheduled a second sleep.
   */
  it("collapses a repeated request into one action, not two", async () => {
    const key = sleepIdempotencyKey(new Date());
    const first = await sleepNow(client, key);
    const second = await sleepNow(client, key);

    expect(first.scheduled).toBe(true);
    expect(first.replay).toBe(false);
    // The device recognised the key and scheduled nothing new.
    expect(second.replay).toBe(true);
    expect(second.scheduled).toBe(false);
    expect(device.performedActions.map((a) => a.action)).toEqual(["sleep"]);
  });

  it("derives the same key for two clicks inside the window", () => {
    const a = sleepIdempotencyKey(new Date("2026-09-14T10:00:00.000Z"));
    const b = sleepIdempotencyKey(new Date("2026-09-14T10:00:41.000Z"));
    expect(b).toBe(a);
  });

  it("derives a different key once the window has passed", () => {
    const a = sleepIdempotencyKey(new Date("2026-09-14T10:00:00.000Z"));
    const b = sleepIdempotencyKey(new Date("2026-09-14T10:02:00.000Z"));
    expect(b).not.toBe(a);
  });

  /**
   * A 200 that scheduled nothing is the device declining, not obeying. The
   * audit entry used to be a flat ok/deviceConfirmed:true whatever came back,
   * which put a line in the log saying the device went to sleep when it
   * demonstrably had not.
   */
  it("audits a declined sleep as refused rather than confirmed", async () => {
    device.actionsScheduleNothing = true;
    const result = await sleepNow(client, sleepIdempotencyKey(new Date()));
    expect(result.scheduled).toBe(false);
    expect(result.replay).toBe(false);

    const entry = readAudit().find((e) => e.action === "device.power.sleep_now");
    expect(entry).toBeDefined();
    expect(entry?.outcome).toBe("refused");
    expect(entry?.deviceConfirmed).toBe(false);
    expect(entry?.detail).toMatch(/scheduled no sleep/i);
  });

  it("audits a scheduled sleep as confirmed", async () => {
    await sleepNow(client, sleepIdempotencyKey(new Date()));
    const entry = readAudit().find((e) => e.action === "device.power.sleep_now");
    expect(entry?.outcome).toBe("ok");
    expect(entry?.deviceConfirmed).toBe(true);
  });
});

describe("waiting is not attempting", () => {
  /**
   * The scheduler comes round every thirty seconds whether or not the device
   * is there. Counting each of those passes as a delivery attempt made an
   * intent waiting out one hour of sleep accumulate a hundred and twenty
   * "attempts" without a packet being sent — which the UI then showed next to
   * the request, reading as a device that had refused a hundred and twenty
   * times rather than a tower that had patiently waited once.
   */
  it("does not count a pass at a sleeping device as an attempt", async () => {
    device.asleep = true;
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    expect(readIntent()?.attempts).toBe(0);

    for (let i = 0; i < 5; i += 1) {
      const { status, reachable } = await readStatus();
      const result = await reconcileIntent(client, status, { reachable });
      expect(result.disposition.kind).toBe("waiting");
    }

    const after = readIntent();
    expect(after?.attempts).toBe(0);
    // And nothing at all was written, so there is no churn either.
    expect(after?.lastAttemptAt).toBeNull();
  });

  it("does count a pass that actually wrote to the device", async () => {
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    const { status, reachable } = await readStatus();
    const result = await reconcileIntent(client, status, { reachable });
    expect(result.applied).toBe(true);
    expect(result.intent?.attempts).toBe(1);
  });

  it("counts a failed write as an attempt", async () => {
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    const { status } = await readStatus();
    device.bumpRevisionLocally((config) => {
      config.gallery.slide_min = 10;
    });
    const result = await reconcileIntent(client, status, { reachable: true });
    expect(result.applied).toBe(false);
    expect(result.intent?.attempts).toBe(1);
    expect(result.intent?.lastAttemptError).toBeTruthy();
  });
});

describe("describing an intent without delivering it", () => {
  /**
   * `GET /api/device/status` used to call reconcileIntent, which meant a plain
   * read of the status page could PATCH the device's configuration and rewrite
   * the tower's durable state — a mutation on a route with no CSRF check.
   * describeIntent runs the same decision and writes nothing.
   */
  it("reaches the same disposition as a reconcile would, and writes nothing", async () => {
    recordIntent({ mode: "always_on", interactiveMinutes: null, wakeIntervalMinutes: null });
    const before = JSON.stringify(readState());
    const { status, reachable } = await readStatus();

    const described = describeIntent(status, { reachable });
    expect(described.disposition.kind).toBe("apply");
    expect(described.applied).toBe(false);

    // The device was not written to...
    expect(device.config.power.mode).toBe("auto_saver");
    // ...and neither was the tower's state.
    expect(JSON.stringify(readState())).toBe(before);
  });

  it("describes a sleeping device as waiting without touching anything", async () => {
    device.asleep = true;
    recordIntent({ mode: "interactive", interactiveMinutes: 30, wakeIntervalMinutes: null });
    const before = JSON.stringify(readState());

    const described = describeIntent(null, { reachable: false });
    expect(described.disposition.kind).toBe("waiting");
    expect(described.detail).toMatch(/sleeping device/i);
    expect(JSON.stringify(readState())).toBe(before);
  });

  it("does not clear an expired intent, because describing is not deciding", () => {
    recordIntent({
      mode: "interactive",
      interactiveMinutes: 30,
      wakeIntervalMinutes: null,
      now: new Date("2026-09-14T00:00:00.000Z"),
    });
    const described = describeIntent(null, {
      reachable: false,
      now: new Date("2026-09-14T09:00:00.000Z"),
    });
    expect(described.disposition.kind).toBe("expired");
    // Still on disk: only the reconcile path may clear it.
    expect(readIntent()).not.toBeNull();
  });
});

describe("the status read that drives all of this", () => {
  it("reports a usable battery when the device has one", async () => {
    const status = await client.status();
    expect(status.power?.battery.mv).toBe(3912);
    expect(status.power?.battery.percent).toBe(57);
  });

  it("reports null for an uncalibrated battery, never a number", async () => {
    device.batteryCalibrated = false;
    const status = await client.status();
    expect(status.power?.battery.calibrated).toBe(false);
    expect(status.power?.battery.mv).toBeNull();
    expect(status.power?.battery.percent).toBeNull();
  });

  it("reports null for an implausible voltage", async () => {
    device.batteryMillivolts = 1200;
    const status = await client.status();
    expect(status.power?.battery.plausible).toBe(false);
    expect(status.power?.battery.mv).toBeNull();
    expect(status.power?.battery.percent).toBeNull();
  });

  it("parses a device that sends no power block at all", async () => {
    await device.close();
    device = new MockDevice({
      token: TOKEN,
      capabilities: ["dashboard.frame.v1", "config.v2"],
    });
    const origin = await device.listen(0);
    client = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: origin,
      token: TOKEN,
    });
    const status = await client.status();
    // Absent, not invented. The tower gates on the capability list.
    expect(status.power).toBeNull();
  });
});

describe("the mock can be driven into the states the tower has to handle", () => {
  /**
   * Every knob here exists because some tower behaviour is only reachable when
   * the device says something specific. A mock that could only ever answer
   * "acknowledged, no timer armed, outcome updated" left those paths
   * untestable, which is how the tower came to have rendering for states no
   * test had ever produced.
   */
  it("can report a request it has heard but not acted on", async () => {
    device.powerDesiredMode = "always_on";
    const status = await client.status();
    expect(status.power?.mode).toBe("auto_saver");
    expect(status.power?.desired_mode).toBe("always_on");
    expect(status.power?.ack).toBe("pending_wake");
  });

  it("can report an armed timer with a real countdown", async () => {
    device.powerTimerArmed = true;
    device.powerNextWakeInS = 1800;
    device.powerNextWakeEpoch = 1789000000;
    const status = await client.status();
    expect(status.power?.timer_armed).toBe(true);
    expect(status.power?.next_wake_in_s).toBe(1800);
    expect(status.power?.next_wake_epoch).toBe(1789000000);
  });

  /**
   * A device whose clock has never been set cannot name a wall-clock time, and
   * the tower must render that as an estimate rather than as a reported fact.
   */
  it("can report an armed timer on a device with no clock", async () => {
    device.powerTimerArmed = true;
    device.powerNextWakeInS = 900;
    device.powerNextWakeEpoch = null;
    const status = await client.status();
    expect(status.power?.timer_armed).toBe(true);
    expect(status.power?.next_wake_epoch).toBeNull();
  });

  it("can report a device that has not finished a cycle", async () => {
    device.lastOutcome = null;
    const status = await client.status();
    expect(status.power?.last_outcome).toBeNull();
  });

  it("can report which phase the wake budget ran out in", async () => {
    device.lastOutcome = "budget_exhausted";
    device.budgetExhaustedPhase = "network";
    device.consecutiveFailures = 3;
    const status = await client.status();
    expect(status.power?.last_outcome).toBe("budget_exhausted");
    expect(status.power?.budget_exhausted_phase).toBe("network");
    expect(status.power?.consecutive_failures).toBe(3);
  });

  it("can accept an action and schedule nothing, the way a busy device does", async () => {
    device.actionsScheduleNothing = true;
    const result = await client.runAction("sleep", sleepIdempotencyKey(new Date()));
    expect(result.scheduled).toBe(false);
    expect(result.replay).toBe(false);
    expect(device.performedActions).toHaveLength(0);
  });
});

describe("changing the interactive window length", () => {
  /**
   * Mirrors the firmware fix in device_config_service.cc: a patch carrying
   * only `power.interactive_min` sets how long the *next* window will be and
   * must not close the one that is open. Routing it through the mode change is
   * how a user adjusting the length of a window was thrown out of it.
   */
  it("leaves an open window open on the device", async () => {
    // Open a window by asking for interactive.
    await client.patchConfig(await revision(), { "power.mode": "interactive" });
    const during = await client.status();
    expect(during.power?.mode).toBe("interactive");
    const remaining = during.power?.interactive_remaining_s ?? 0;
    expect(remaining).toBeGreaterThan(0);

    // Now change only the length.
    await client.patchConfig(await revision(), { "power.interactive_min": 60 });

    const after = await client.status();
    expect(after.power?.mode).toBe("interactive");
    expect(after.power?.interactive_remaining_s ?? 0).toBeGreaterThan(0);
    // The new length is stored for next time...
    expect(device.config.power.interactive_min).toBe(60);
    // ...and the base mode is still the saver, because a window is not a mode.
    expect(device.config.power.mode).toBe("auto_saver");
  });

  it("uses the new length for the window after it", async () => {
    await client.patchConfig(await revision(), { "power.interactive_min": 5 });
    await client.patchConfig(await revision(), { "power.mode": "interactive" });
    const status = await client.status();
    // Five minutes, not the fifteen it started with.
    expect(status.power?.interactive_remaining_s ?? 0).toBeLessThanOrEqual(300);
    expect(status.power?.interactive_remaining_s ?? 0).toBeGreaterThan(240);
  });

  /**
   * `interactive` is a live window with a deadline, never a stored mode. The
   * config route must report the base mode or it promises a reader a mode the
   * device will not be in after a reboot.
   */
  it("never reports interactive as the stored configuration", async () => {
    await client.patchConfig(await revision(), { "power.mode": "interactive" });
    const config = await client.getConfig();
    expect(config.config.power?.mode).toBe("auto_saver");
    const status = await client.status();
    expect(status.power?.mode).toBe("interactive");
  });
});

/** The device's current config revision, for a compare-and-swap write. */
async function revision(): Promise<number> {
  const status = await client.status();
  return status.config_revision as number;
}
