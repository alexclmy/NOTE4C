import { describe, expect, it } from "vitest";
import { NO_REMOTE_WAKE_NOTE } from "@/core/power";
import {
  MAX_QUEUED_PUSH_ATTEMPTS,
  QUEUED_PUSH_MAX_AGE_MS,
  QUEUED_RETRY_BASE_MS,
  QUEUED_RETRY_MAX_MS,
  describeQueuedPush,
  isTransientPushCode,
  planQueuedPush,
  queuedExpiryReason,
  queuedPushExpiresAt,
  queuedRetryAt,
  queuedRetryDelayMs,
  type QueuedPushFacts,
} from "@/core/pushQueue";

const NOW = new Date("2026-09-14T10:00:00.000Z");

function queued(overrides: Partial<QueuedPushFacts> = {}): QueuedPushFacts {
  return {
    queuedAt: NOW.toISOString(),
    attempts: 0,
    lastAttemptAt: null,
    lastError: null,
    ...overrides,
  };
}

function at(offsetMs: number): Date {
  return new Date(NOW.getTime() + offsetMs);
}

describe("which failures may be queued", () => {
  it("queues only the two that mean 'not now'", () => {
    expect(isTransientPushCode("unreachable")).toBe(true);
    expect(isTransientPushCode("busy")).toBe(true);
  });

  /**
   * The whole point of the closed list. A device that answered and said no has
   * told the tower something a retry cannot change, and hiding that behind a
   * queue turns a five-second fix into an hour of a spinner.
   */
  it("never queues a refusal, a configuration error or a validation failure", () => {
    for (const code of [
      "unauthorized",
      "not_provisioned",
      "locked_out",
      "no_token",
      "sha_mismatch",
      "bad_length",
      "too_large",
      "lockdown",
      "bad_put_shape",
      "http_500",
      "unknown",
    ]) {
      expect(isTransientPushCode(code), `${code} must not be queued`).toBe(false);
    }
  });
});

describe("planQueuedPush", () => {
  it("says nothing when nothing is queued", () => {
    expect(planQueuedPush({ queued: null, reachable: true, now: NOW })).toEqual({
      kind: "none",
    });
  });

  it("waits, silently, while the device is not answering", () => {
    const plan = planQueuedPush({ queued: queued(), reachable: false, now: NOW });
    expect(plan.kind).toBe("waiting");
    if (plan.kind !== "waiting") throw new Error("unreachable");
    expect(plan.reason).toMatch(/held here/);
    // Never a claim that anything here can wake it.
    expect(plan.reason).not.toMatch(/wake the device|waking it/i);
  });

  it("sends as soon as the device answers", () => {
    expect(planQueuedPush({ queued: queued(), reachable: true, now: NOW })).toEqual({
      kind: "send",
    });
  });

  /**
   * Waiting is free and must stay free: the scheduler comes round every thirty
   * seconds whether or not the device is there, so a plan that treated a wait
   * as an attempt would exhaust the ceiling in four minutes of nobody home.
   */
  it("keeps waiting however long the device is away, as long as it is young", () => {
    const old = queued({ queuedAt: new Date(NOW.getTime() - 5 * 3_600_000).toISOString() });
    expect(planQueuedPush({ queued: old, reachable: false, now: NOW }).kind).toBe(
      "waiting",
    );
  });

  it("backs off, with a delay that doubles, after a wire attempt that did not land", () => {
    const once = queued({
      attempts: 1,
      lastAttemptAt: NOW.toISOString(),
      lastError: "The device could not be reached",
    });
    const plan = planQueuedPush({ queued: once, reachable: true, now: at(10_000) });
    expect(plan.kind).toBe("backoff");
    if (plan.kind !== "backoff") throw new Error("unreachable");
    expect(new Date(plan.retryAt).getTime()).toBe(NOW.getTime() + QUEUED_RETRY_BASE_MS);
    expect(plan.reason).toMatch(/50 seconds/);
    expect(plan.reason).toContain("The device could not be reached");

    // And once the delay has run out it sends.
    expect(
      planQueuedPush({ queued: once, reachable: true, now: at(QUEUED_RETRY_BASE_MS) }).kind,
    ).toBe("send");
  });

  it("expires on age, and says the frame was never sent", () => {
    const stale = queued({
      queuedAt: new Date(NOW.getTime() - QUEUED_PUSH_MAX_AGE_MS - 60_000).toISOString(),
    });
    const plan = planQueuedPush({ queued: stale, reachable: true, now: NOW });
    expect(plan.kind).toBe("expired");
    if (plan.kind !== "expired") throw new Error("unreachable");
    expect(plan.reason).toMatch(/6 hours/);
    expect(plan.reason).toMatch(/Nothing was sent/);
  });

  it("expires on attempts, and names them", () => {
    const exhausted = queued({
      attempts: MAX_QUEUED_PUSH_ATTEMPTS,
      lastAttemptAt: NOW.toISOString(),
      lastError: "The device could not be reached",
    });
    const plan = planQueuedPush({ queued: exhausted, reachable: true, now: NOW });
    expect(plan.kind).toBe("expired");
    if (plan.kind !== "expired") throw new Error("unreachable");
    expect(plan.reason).toMatch(new RegExp(`${MAX_QUEUED_PUSH_ATTEMPTS} times`));
  });

  /**
   * Expiry beats everything, including an unreachable device. A queued push
   * that has run out is dropped whether or not anybody is home to receive it,
   * or it would sit in the blocking set forever waiting for a device that is
   * never coming back.
   */
  it("expires even while the device is away", () => {
    const stale = queued({
      queuedAt: new Date(NOW.getTime() - QUEUED_PUSH_MAX_AGE_MS - 1).toISOString(),
    });
    expect(planQueuedPush({ queued: stale, reachable: false, now: NOW }).kind).toBe(
      "expired",
    );
  });
});

describe("the arithmetic", () => {
  it("doubles the delay and caps it", () => {
    expect(queuedRetryDelayMs(0)).toBe(0);
    expect(queuedRetryDelayMs(1)).toBe(QUEUED_RETRY_BASE_MS);
    expect(queuedRetryDelayMs(2)).toBe(QUEUED_RETRY_BASE_MS * 2);
    expect(queuedRetryDelayMs(3)).toBe(QUEUED_RETRY_BASE_MS * 4);
    expect(queuedRetryDelayMs(40)).toBe(QUEUED_RETRY_MAX_MS);
  });

  it("has a bounded worst case: every attempt fits inside the expiry", () => {
    let total = 0;
    for (let n = 1; n <= MAX_QUEUED_PUSH_ATTEMPTS; n += 1) total += queuedRetryDelayMs(n);
    expect(total).toBeLessThan(QUEUED_PUSH_MAX_AGE_MS);
  });

  it("reports no retry time for a push that has never been attempted", () => {
    expect(queuedRetryAt(queued())).toBeNull();
    expect(queuedRetryAt(queued({ attempts: 2, lastAttemptAt: "not a date" }))).toBeNull();
  });

  it("reports the expiry moment, and nothing for a broken timestamp", () => {
    expect(queuedPushExpiresAt(NOW.toISOString())?.getTime()).toBe(
      NOW.getTime() + QUEUED_PUSH_MAX_AGE_MS,
    );
    expect(queuedPushExpiresAt("never")).toBeNull();
    expect(queuedExpiryReason(queued({ queuedAt: "never" }), NOW)).toBeNull();
  });
});

describe("the sentence the interface shows", () => {
  const note = NO_REMOTE_WAKE_NOTE;

  it("says the frame is held, when it will next be reachable, and that nothing here wakes it", () => {
    const text = describeQueuedPush({
      queued: queued(),
      reachable: false,
      deviceMode: "real",
      nextWakeLabel: "at 11:04 (estimated)",
      noRemoteWakeNote: note,
      now: NOW,
    });
    expect(text).toMatch(/held here/);
    expect(text).toMatch(/next expected to be reachable at 11:04 \(estimated\)/);
    expect(text).toContain(note);
    expect(text).toMatch(/drops it rather than paint/);
  });

  it("never claims the network can wake a sleeping device", () => {
    const text = describeQueuedPush({
      queued: queued({ attempts: 2, lastError: "busy" }),
      reachable: false,
      deviceMode: "real",
      nextWakeLabel: null,
      noRemoteWakeNote: note,
      now: NOW,
    });
    expect(text).not.toMatch(/wake the device|waking the device|wake it up/i);
    expect(text).toMatch(/2 attempts did not land/);
  });

  it("does not talk about sleep schedules for a mock, which has none", () => {
    const text = describeQueuedPush({
      queued: queued(),
      reachable: false,
      deviceMode: "mock",
      nextWakeLabel: "at 11:04 (estimated)",
      noRemoteWakeNote: note,
      now: NOW,
    });
    expect(text).toMatch(/mock does not sleep/);
    expect(text).not.toContain("11:04");
  });

  it("says plainly when the device is back and the frame is about to go", () => {
    const text = describeQueuedPush({
      queued: queued(),
      reachable: true,
      deviceMode: "real",
      nextWakeLabel: null,
      noRemoteWakeNote: note,
      now: NOW,
    });
    expect(text).toMatch(/goes out on the next pass/);
  });
});
