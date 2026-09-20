/**
 * The hybrid low-power model, tower side.
 *
 * The claim these tests exist to defend is the one the whole feature turns on:
 * **the tower never pretends it woke a sleeping device.** A request aimed at an
 * unreachable device becomes a pending intent with an honest sentence, and it
 * is delivered when the device is next actually there — not before, and not
 * with a success message in the meantime.
 */

import { describe, expect, it } from "vitest";
import {
  DEFAULT_WAKE_INTERVAL_MIN,
  DURABLE_INTENT_MAX_AGE_MS,
  INTENT_RETRY_BASE_MS,
  INTENT_RETRY_MAX_MS,
  INTERACTIVE_INTENT_MAX_AGE_MS,
  MAX_INTENT_ATTEMPTS,
  INTERACTIVE_MINUTES,
  MODE_COPY,
  NO_REMOTE_WAKE_NOTE,
  POWER_CAPABILITY,
  POWER_MODES,
  batteryUnavailableReason,
  describeReachability,
  deviceSupportsPower,
  estimateNextWake,
  intentExpiryReason,
  intentIsSatisfied,
  intentIsStale,
  intentRetryAt,
  intentRetryDelayMs,
  intentToPatch,
  isInteractiveMinutes,
  sleepIdempotencyKey,
  DevicePowerSchema,
  isPowerMode,
  isWakeIntervalMinutes,
  planIntent,
  type DevicePower,
  type PowerIntent,
} from "@/core/power";

function power(overrides: Partial<DevicePower> = {}): DevicePower {
  return {
    contract: 1,
    mode: "auto_saver",
    desired_mode: "auto_saver",
    ack: "acknowledged",
    awake: true,
    sleep_intent: true,
    interactive_remaining_s: 0,
    wake_interval_min: 60,
    timer_armed: false,
    next_wake_in_s: 0,
    next_wake_epoch: null,
    last_wake_reason: "timer",
    last_outcome: "updated",
    budget_exhausted_phase: null,
    consecutive_failures: 0,
    battery: {
      present: true,
      calibrated: true,
      plausible: true,
      mv: 3912,
      percent: 57,
    },
    charge: { state: "no_power", charging: false },
    ...overrides,
  };
}

function intent(overrides: Partial<PowerIntent> = {}): PowerIntent {
  return {
    mode: "interactive",
    interactiveMinutes: 15,
    wakeIntervalMinutes: null,
    requestedAt: "2026-09-14T10:00:00.000Z",
    appliedAt: null,
    lastAttemptAt: null,
    lastAttemptError: null,
    attempts: 0,
    ...overrides,
  };
}

const NOW = new Date("2026-09-14T10:05:00.000Z");

describe("mode and bound validation", () => {
  it("accepts exactly the three wire names the firmware parses", () => {
    expect(POWER_MODES).toEqual(["auto_saver", "interactive", "always_on"]);
    for (const mode of POWER_MODES) expect(isPowerMode(mode)).toBe(true);
    // Case matters on the wire, and neighbours are not quietly accepted.
    expect(isPowerMode("AUTO_SAVER")).toBe(false);
    expect(isPowerMode("auto")).toBe(false);
    expect(isPowerMode("sleep")).toBe(false);
    expect(isPowerMode(1)).toBe(false);
    expect(isPowerMode(null)).toBe(false);
  });

  it("accepts exactly the four interactive windows", () => {
    expect(INTERACTIVE_MINUTES).toEqual([5, 15, 30, 60]);
    for (const m of INTERACTIVE_MINUTES) expect(isInteractiveMinutes(m)).toBe(true);
    for (const m of [0, 1, 10, 45, 120, -5]) {
      expect(isInteractiveMinutes(m)).toBe(false);
    }
  });

  it("floors the wake interval so the saver still saves", () => {
    expect(isWakeIntervalMinutes(15)).toBe(true);
    expect(isWakeIntervalMinutes(60)).toBe(true);
    expect(isWakeIntervalMinutes(1440)).toBe(true);
    // Below the floor the radio and panel dominate the average current.
    expect(isWakeIntervalMinutes(5)).toBe(false);
    expect(isWakeIntervalMinutes(0)).toBe(false);
    expect(isWakeIntervalMinutes(1441)).toBe(false);
    expect(isWakeIntervalMinutes(60.5)).toBe(false);
  });

  it("warns about always_on and about nothing else", () => {
    // The one choice that changes battery life by two orders of magnitude has
    // to say so next to the button, not in a manual.
    expect(MODE_COPY.always_on.warning).toBeTruthy();
    expect(MODE_COPY.always_on.warning).toMatch(/battery/i);
    expect(MODE_COPY.auto_saver.warning).toBeNull();
    expect(MODE_COPY.interactive.warning).toBeNull();
  });
});

describe("capability gating", () => {
  it("requires the capability rather than assuming it", () => {
    expect(deviceSupportsPower([POWER_CAPABILITY])).toBe(true);
    expect(deviceSupportsPower(["config.v2"])).toBe(false);
    // An api 1 device sends no list at all. Assuming support and finding out
    // from a 400 would mean writing to a device that cannot honour it.
    expect(deviceSupportsPower(undefined)).toBe(false);
    expect(deviceSupportsPower([])).toBe(false);
  });
});

describe("battery honesty", () => {
  it("says nothing is wrong when the reading is usable", () => {
    expect(batteryUnavailableReason(power())).toBeNull();
  });

  it("explains an uncalibrated chip rather than showing a number", () => {
    const p = power({
      battery: { present: true, calibrated: false, plausible: true, mv: null, percent: null },
    });
    expect(batteryUnavailableReason(p)).toMatch(/calibration/i);
  });

  it("explains an implausible voltage", () => {
    const p = power({
      battery: { present: true, calibrated: true, plausible: false, mv: null, percent: null },
    });
    expect(batteryUnavailableReason(p)).toMatch(/outside the range/i);
  });

  it("explains a missing reading", () => {
    const p = power({
      battery: { present: false, calibrated: true, plausible: false, mv: null, percent: null },
    });
    expect(batteryUnavailableReason(p)).toMatch(/did not return/i);
  });
});

describe("intent to patch", () => {
  it("sends the window length only for interactive", () => {
    expect(intentToPatch(intent({ mode: "interactive", interactiveMinutes: 30 }))).toEqual({
      "power.mode": "interactive",
      "power.interactive_min": 30,
    });
    expect(intentToPatch(intent({ mode: "auto_saver", interactiveMinutes: 30 }))).toEqual({
      "power.mode": "auto_saver",
    });
  });

  it("sends the wake interval only when the user changed it", () => {
    // A null means "leave the device's value alone", which is different from
    // writing the tower's default over a value somebody chose on the device.
    expect(
      intentToPatch(intent({ mode: "auto_saver", wakeIntervalMinutes: null })),
    ).not.toHaveProperty("power.wake_interval_min");
    expect(
      intentToPatch(intent({ mode: "auto_saver", wakeIntervalMinutes: 120 })),
    ).toMatchObject({ "power.wake_interval_min": 120 });
  });
});

describe("intentIsSatisfied", () => {
  it("is satisfied when the device is already in a durable mode", () => {
    expect(intentIsSatisfied(intent({ mode: "auto_saver" }), power({ mode: "auto_saver" }))).toBe(true);
    expect(intentIsSatisfied(intent({ mode: "always_on" }), power({ mode: "auto_saver" }))).toBe(false);
  });

  it("is never satisfied by an interactive window that is already open", () => {
    // Somebody asking for thirty minutes wants thirty minutes from now, not
    // whatever is left of a window that opened twenty minutes ago.
    const open = power({ mode: "interactive", interactive_remaining_s: 600 });
    expect(intentIsSatisfied(intent({ mode: "interactive" }), open)).toBe(false);
  });

  it("is not satisfied when the wake interval still differs", () => {
    expect(
      intentIsSatisfied(
        intent({ mode: "auto_saver", wakeIntervalMinutes: 120 }),
        power({ mode: "auto_saver", wake_interval_min: 60 }),
      ),
    ).toBe(false);
  });
});

describe("intent staleness", () => {
  it("expires an interactive request the device never picked up", () => {
    const old = intent({ requestedAt: "2026-09-14T00:00:00.000Z" });
    // Waking into an interactive window six hours after the person gave up
    // would drain the battery for nobody.
    expect(intentIsStale(old, new Date("2026-09-14T07:00:00.000Z"))).toBe(true);
    expect(intentIsStale(old, new Date("2026-09-14T05:00:00.000Z"))).toBe(false);
  });

  it("keeps a durable preference for days, then gives up on it honestly", () => {
    // "Put it in power saving" is still true tomorrow, so a durable intent
    // outlives an interactive one by a long way — but not forever. An intent
    // is a line in the tower's list of things it still owes the device, and it
    // is drawn as PENDING on every page; one that nothing will ever deliver is
    // a badge about a thing that is never going to happen. These used to have
    // no expiry at all.
    const base = new Date("2026-09-01T00:00:00.000Z");
    for (const mode of ["auto_saver", "always_on"] as const) {
      const held = intent({ mode, requestedAt: base.toISOString() });
      // Well past the six hours an interactive request gets.
      expect(
        intentIsStale(held, new Date(base.getTime() + 24 * 60 * 60 * 1000)),
        mode,
      ).toBe(false);
      expect(
        intentIsStale(held, new Date(base.getTime() + DURABLE_INTENT_MAX_AGE_MS)),
        mode,
      ).toBe(false);
      expect(
        intentIsStale(
          held,
          new Date(base.getTime() + DURABLE_INTENT_MAX_AGE_MS + 1_000),
        ),
        mode,
      ).toBe(true);
    }
  });

  it("says which of the two ways out it took, in words", () => {
    const base = new Date("2026-09-01T00:00:00.000Z");
    const aged = intent({
      mode: "auto_saver",
      requestedAt: base.toISOString(),
    });
    expect(
      intentExpiryReason(
        aged,
        new Date(base.getTime() + DURABLE_INTENT_MAX_AGE_MS + 1_000),
      ),
    ).toMatch(/without the device ever being reachable/);

    // The other way out: the device WAS there and kept refusing. A different
    // failure, and a reader needs to be able to tell them apart.
    const refused = intent({
      mode: "auto_saver",
      requestedAt: base.toISOString(),
      attempts: MAX_INTENT_ATTEMPTS,
      lastAttemptAt: base.toISOString(),
      lastAttemptError: "The device refused the write",
    });
    const reason = intentExpiryReason(refused, new Date(base.getTime() + 60_000));
    expect(reason).toMatch(/reachable and refused this change/);
    expect(reason).toMatch(/The device refused the write/);
  });

  it("gives up after a bounded number of real write attempts", () => {
    const base = new Date("2026-09-01T00:00:00.000Z");
    const nearly = intent({
      mode: "always_on",
      requestedAt: base.toISOString(),
      attempts: MAX_INTENT_ATTEMPTS - 1,
      lastAttemptAt: base.toISOString(),
    });
    expect(intentIsStale(nearly, new Date(base.getTime() + 60_000))).toBe(false);
    expect(
      intentIsStale(
        { ...nearly, attempts: MAX_INTENT_ATTEMPTS },
        new Date(base.getTime() + 60_000),
      ),
    ).toBe(true);
  });

  it("never expires an intent that was already delivered", () => {
    const done = intent({
      requestedAt: "2026-09-14T00:00:00.000Z",
      appliedAt: "2026-09-14T00:00:05.000Z",
    });
    expect(intentIsStale(done, new Date("2026-09-20T00:00:00.000Z"))).toBe(false);
  });

  it("uses the six hour bound the constant advertises", () => {
    const base = new Date("2026-09-14T00:00:00.000Z");
    const i = intent({ requestedAt: base.toISOString() });
    expect(intentIsStale(i, new Date(base.getTime() + INTERACTIVE_INTENT_MAX_AGE_MS))).toBe(false);
    expect(intentIsStale(i, new Date(base.getTime() + INTERACTIVE_INTENT_MAX_AGE_MS + 1))).toBe(true);
  });
});

describe("planIntent", () => {
  it("does nothing when there is no intent", () => {
    expect(
      planIntent({ intent: null, power: power(), reachable: true, supported: true, now: NOW }),
    ).toEqual({ kind: "none" });
  });

  it("does nothing for an intent already delivered", () => {
    expect(
      planIntent({
        intent: intent({ appliedAt: NOW.toISOString() }),
        power: power(),
        reachable: true,
        supported: true,
        now: NOW,
      }),
    ).toEqual({ kind: "none" });
  });

  /**
   * The headline behaviour. An unreachable device does not produce an error,
   * a retry, or a claim of success: it produces a wait, and a sentence that
   * says a remote wake does not exist.
   */
  it("waits when the device is asleep, and says why without promising a wake", () => {
    const plan = planIntent({
      intent: intent(),
      power: null,
      reachable: false,
      supported: true,
      now: NOW,
    });
    expect(plan.kind).toBe("waiting");
    if (plan.kind !== "waiting") throw new Error("unreachable");
    expect(plan.reason).toMatch(/nothing on the network can wake it/i);
    expect(plan.reason).toMatch(/button/i);
    // And it must never suggest the tower can do it.
    expect(plan.reason).not.toMatch(/waking the device/i);
  });

  it("waits when reachable is true but no power block came back", () => {
    // A half-read status is not a device that answered about its power state.
    const plan = planIntent({
      intent: intent(),
      power: null,
      reachable: true,
      supported: true,
      now: NOW,
    });
    expect(plan.kind).toBe("waiting");
  });

  it("applies when the device is there", () => {
    const plan = planIntent({
      intent: intent({ mode: "interactive", interactiveMinutes: 30 }),
      power: power(),
      reachable: true,
      supported: true,
      now: NOW,
    });
    expect(plan).toEqual({
      kind: "apply",
      set: { "power.mode": "interactive", "power.interactive_min": 30 },
    });
  });

  it("clears an intent the device already satisfies", () => {
    const plan = planIntent({
      intent: intent({ mode: "auto_saver", interactiveMinutes: null }),
      power: power({ mode: "auto_saver" }),
      reachable: true,
      supported: true,
      now: NOW,
    });
    expect(plan.kind).toBe("satisfied");
  });

  it("expires a stale interactive intent rather than applying it late", () => {
    const plan = planIntent({
      intent: intent({ requestedAt: "2026-09-14T00:00:00.000Z" }),
      power: power(),
      reachable: true,
      supported: true,
      now: new Date("2026-09-14T09:00:00.000Z"),
    });
    expect(plan.kind).toBe("expired");
  });

  it("checks staleness before reachability, so an old intent is not held forever", () => {
    const plan = planIntent({
      intent: intent({ requestedAt: "2026-09-01T00:00:00.000Z" }),
      power: null,
      reachable: false,
      supported: true,
      now: NOW,
    });
    expect(plan.kind).toBe("expired");
  });

  it("expires against a device whose firmware has no power contract", () => {
    const plan = planIntent({
      intent: intent({ mode: "always_on" }),
      power: power(),
      reachable: true,
      supported: false,
      now: NOW,
    });
    expect(plan.kind).toBe("expired");
    if (plan.kind !== "expired") throw new Error("unreachable");
    expect(plan.reason).toMatch(/does not advertise/i);
  });
});

describe("estimateNextWake", () => {
  it("prefers the device's own answer and marks it as a fact", () => {
    const epoch = Math.floor(new Date("2026-09-14T11:00:00.000Z").getTime() / 1000);
    const got = estimateNextWake(power({ timer_armed: true, next_wake_epoch: epoch }), null);
    expect(got).not.toBeNull();
    expect(got?.estimated).toBe(false);
    expect(got?.at.toISOString()).toBe("2026-09-14T11:00:00.000Z");
  });

  it("falls back to arithmetic and marks it as an estimate", () => {
    const seen = new Date("2026-09-14T10:00:00.000Z");
    const got = estimateNextWake(power({ wake_interval_min: 60 }), seen);
    expect(got?.estimated).toBe(true);
    expect(got?.at.toISOString()).toBe("2026-09-14T11:00:00.000Z");
  });

  it("uses the default interval when the device said nothing at all", () => {
    const seen = new Date("2026-09-14T10:00:00.000Z");
    const got = estimateNextWake(null, seen);
    expect(got?.estimated).toBe(true);
    expect(got?.at.getTime()).toBe(seen.getTime() + DEFAULT_WAKE_INTERVAL_MIN * 60_000);
  });

  it("returns null rather than inventing a basis", () => {
    // No device answer and no last-seen time is genuinely no information, and
    // a made-up "next wake" is worse than an empty field.
    expect(estimateNextWake(null, null)).toBeNull();
  });

  it("does not treat an unarmed timer as a fact", () => {
    const seen = new Date("2026-09-14T10:00:00.000Z");
    const got = estimateNextWake(
      power({ timer_armed: false, next_wake_epoch: 1_757_880_000 }),
      seen,
    );
    expect(got?.estimated).toBe(true);
  });
});

describe("describeReachability", () => {
  it("never calls a sleeping device offline or disconnected", () => {
    // Not answering is the designed behaviour most of the time. Language that
    // reads as a fault would train the user to ignore a real one.
    const text = describeReachability(false, null);
    expect(text).toMatch(/normal state/i);
    expect(text.toLowerCase()).not.toContain("offline");
    expect(text.toLowerCase()).not.toContain("disconnected");
    expect(text.toLowerCase()).not.toContain("error");
  });

  it("counts down an open interactive window", () => {
    const text = describeReachability(true, power({ mode: "interactive", interactive_remaining_s: 610 }));
    expect(text).toMatch(/11 more minutes/);
    expect(text).toMatch(/on its own/);
  });

  it("uses a singular minute when there is one left", () => {
    const text = describeReachability(true, power({ mode: "interactive", interactive_remaining_s: 30 }));
    expect(text).toMatch(/1 more minute\b/);
  });

  it("says always_on plainly", () => {
    expect(describeReachability(true, power({ mode: "always_on" }))).toMatch(/stay awake/i);
  });

  it("mentions the imminent sleep in the saver", () => {
    expect(describeReachability(true, power({ sleep_intent: true }))).toMatch(/back to sleep/i);
  });
});

describe("the no-remote-wake note", () => {
  it("names the radio and the button, and promises nothing else", () => {
    expect(NO_REMOTE_WAKE_NOTE).toMatch(/radio/i);
    expect(NO_REMOTE_WAKE_NOTE).toMatch(/button/i);
    expect(NO_REMOTE_WAKE_NOTE).toMatch(/next time it wakes/i);
  });
});

describe("the honest fields the firmware added", () => {
  /**
   * `last_outcome` is null until a wake cycle has actually finished. The
   * firmware used to default it to "updated", which told the tower the last
   * update succeeded on a device that had never completed one — so the schema
   * has to carry the distinction rather than flatten it back out.
   */
  it("accepts a null last_outcome from a device that has not finished a cycle", () => {
    const parsed = DevicePowerSchema.parse({
      ...power(),
      last_outcome: null,
    });
    expect(parsed.last_outcome).toBeNull();
  });

  it("accepts a named outcome", () => {
    const parsed = DevicePowerSchema.parse({
      ...power(),
      last_outcome: "budget_exhausted",
      budget_exhausted_phase: "network",
    });
    expect(parsed.last_outcome).toBe("budget_exhausted");
    expect(parsed.budget_exhausted_phase).toBe("network");
  });

  /**
   * A build that advertises power.hybrid.v1 but predates the phase field omits
   * it, and a missing field carries the same information as a null one.
   */
  it("treats a missing budget_exhausted_phase as null rather than failing", () => {
    const { budget_exhausted_phase: _omitted, ...rest } = power();
    const parsed = DevicePowerSchema.parse(rest);
    expect(parsed.budget_exhausted_phase).toBeNull();
  });
});

describe("the sleep-now idempotency key", () => {
  /**
   * The tower used to mint a fresh randomUUID() per call, which satisfies the
   * firmware's requirement that the header be present while defeating what the
   * header is for: every retry looked like a new action and scheduled another
   * sleep. A key that is not derived from anything is not an idempotency key.
   */
  it("is stable for two requests inside the window", () => {
    const first = sleepIdempotencyKey(new Date("2026-09-14T12:00:00.000Z"));
    const second = sleepIdempotencyKey(new Date("2026-09-14T12:00:30.000Z"));
    expect(second).toBe(first);
  });

  it("changes once the window has passed", () => {
    const first = sleepIdempotencyKey(new Date("2026-09-14T12:00:00.000Z"));
    const later = sleepIdempotencyKey(new Date("2026-09-14T12:05:00.000Z"));
    expect(later).not.toBe(first);
  });

  it("is shaped like the UUID every other caller sends", () => {
    const key = sleepIdempotencyKey(new Date("2026-09-14T12:00:00.000Z"));
    expect(key).toMatch(
      /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/,
    );
  });

  it("honours a caller-supplied window", () => {
    const a = sleepIdempotencyKey(new Date("2026-09-14T12:00:00.000Z"), 1000);
    const b = sleepIdempotencyKey(new Date("2026-09-14T12:00:02.000Z"), 1000);
    expect(b).not.toBe(a);
  });
});


/**
 * The backoff, which did not exist.
 *
 * Before this, a reachable device that refused the write left the intent in
 * place with no delay of any kind, and the scheduler re-sent it on every
 * thirty-second tick: a hundred and twenty writes an hour at a panel that had
 * already said no, each one a socket on a device that serves four and each one
 * a line in the audit log. Every test here fails against that code.
 */
describe("retry backoff", () => {
  it("does not delay anything before the first attempt", () => {
    expect(intentRetryDelayMs(0)).toBe(0);
    expect(intentRetryDelayMs(-1)).toBe(0);
    expect(
      intentRetryAt(intent({ attempts: 0, lastAttemptAt: null })),
    ).toBeNull();
  });

  it("doubles, starting at one scheduler tick", () => {
    expect(intentRetryDelayMs(1)).toBe(INTENT_RETRY_BASE_MS);
    expect(intentRetryDelayMs(2)).toBe(INTENT_RETRY_BASE_MS * 2);
    expect(intentRetryDelayMs(3)).toBe(INTENT_RETRY_BASE_MS * 4);
  });

  it("is bounded, so it never outlasts the device's own wake interval", () => {
    expect(intentRetryDelayMs(50)).toBe(INTENT_RETRY_MAX_MS);
    // And the exponent cannot overflow into Infinity on the way there.
    expect(Number.isFinite(intentRetryDelayMs(10_000))).toBe(true);
    expect(intentRetryDelayMs(10_000)).toBe(INTENT_RETRY_MAX_MS);
  });

  it("refuses to write again until the delay has run out", () => {
    const base = new Date("2026-09-14T00:00:00.000Z");
    const failed = intent({
      mode: "always_on",
      requestedAt: base.toISOString(),
      attempts: 2,
      lastAttemptAt: base.toISOString(),
      lastAttemptError: "The device refused the write",
    });
    const input = {
      intent: failed,
      power: power({ mode: "auto_saver" }),
      reachable: true,
      supported: true,
    };

    const during = planIntent({
      ...input,
      now: new Date(base.getTime() + INTENT_RETRY_BASE_MS),
    });
    expect(during.kind).toBe("backoff");
    if (during.kind === "backoff") {
      expect(during.reason).toMatch(/refused this change 2 times/);
      expect(during.retryAt).toBe(
        new Date(base.getTime() + INTENT_RETRY_BASE_MS * 2).toISOString(),
      );
    }

    const after = planIntent({
      ...input,
      now: new Date(base.getTime() + INTENT_RETRY_BASE_MS * 2 + 1),
    });
    expect(after.kind).toBe("apply");
  });

  it("does not hold back an intent that has never been attempted", () => {
    expect(
      planIntent({
        intent: intent({ mode: "always_on", attempts: 0, lastAttemptAt: null }),
        power: power({ mode: "auto_saver" }),
        reachable: true,
        supported: true,
        now: NOW,
      }).kind,
    ).toBe("apply");
  });

  it("expires rather than backing off once the attempts run out", () => {
    const base = new Date("2026-09-14T00:00:00.000Z");
    const spent = planIntent({
      intent: intent({
        mode: "always_on",
        requestedAt: base.toISOString(),
        attempts: MAX_INTENT_ATTEMPTS,
        lastAttemptAt: base.toISOString(),
        lastAttemptError: "The device refused the write",
      }),
      power: power({ mode: "auto_saver" }),
      reachable: true,
      supported: true,
      now: new Date(base.getTime() + INTENT_RETRY_MAX_MS * 2),
    });
    expect(spent.kind).toBe("expired");
  });

  it("says waiting, not backing off, when the device is not there at all", () => {
    // The two are different facts and the sentences differ: one is about a
    // device that is asleep, the other about not hammering one that is awake.
    const base = new Date("2026-09-14T00:00:00.000Z");
    const disposition = planIntent({
      intent: intent({
        mode: "always_on",
        requestedAt: base.toISOString(),
        attempts: 2,
        lastAttemptAt: base.toISOString(),
      }),
      power: null,
      reachable: false,
      supported: true,
      now: new Date(base.getTime() + 1_000),
    });
    expect(disposition.kind).toBe("waiting");
  });
});
