/**
 * One vocabulary for "what is the device doing", exhaustively.
 *
 * The defect this replaces: three pages each decided their own wording, so a
 * device that was asleep on purpose was drawn with the same yellow badge as a
 * request waiting to be delivered, and the red UNREACHABLE was shown for the
 * product's *nominal* state. These tests pin the five words and, more
 * importantly, the order they win in — because every interesting case is one
 * where two of them could apply.
 */

import { describe, expect, it } from "vitest";
import {
  DEVICE_STATES,
  OVERDUE_GRACE_MS,
  deriveDeviceState,
  type DeviceState,
  type DeviceStateInput,
  type DevicePower,
  type PowerIntent,
} from "@/core/power";

const NOW = new Date("2026-09-14T12:00:00.000Z");

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
    battery: { present: true, calibrated: true, plausible: true, mv: 3912, percent: 57 },
    charge: { state: "no_power", charging: false },
    ...overrides,
  };
}

function intent(overrides: Partial<PowerIntent> = {}): PowerIntent {
  return {
    mode: "interactive",
    interactiveMinutes: 15,
    wakeIntervalMinutes: null,
    requestedAt: new Date(NOW.getTime() - 60_000).toISOString(),
    appliedAt: null,
    lastAttemptAt: null,
    lastAttemptError: null,
    attempts: 0,
    ...overrides,
  };
}

/** The shape every page hands in. Defaults are "a real device, asleep". */
function input(overrides: Partial<DeviceStateInput> = {}): DeviceStateInput {
  return {
    reachable: false,
    power: null,
    powerSupported: true,
    intent: null,
    deviceMode: "real" as const,
    nextWake: {
      at: new Date(NOW.getTime() + 30 * 60_000).toISOString(),
      estimated: true,
    },
    deviceLastSeenAt: new Date(NOW.getTime() - 30 * 60_000).toISOString(),
    now: NOW,
    ...overrides,
  };
}

describe("deriveDeviceState", () => {
  it("names exactly five states and always explains itself", () => {
    expect([...DEVICE_STATES]).toEqual([
      "awake",
      "asleep",
      "pending",
      "uncertain",
      "unreachable",
    ]);
    for (const state of DEVICE_STATES) {
      // Every reading carries a sentence. A badge with no reason is how the
      // old UI ended up showing a colour nobody could account for.
      const reading = deriveDeviceState(input({ reachable: state === "awake" }));
      expect(reading.reason.length).toBeGreaterThan(20);
    }
  });

  describe("a device that answered", () => {
    it("is awake", () => {
      const reading = deriveDeviceState(input({ reachable: true, power: power() }));
      expect(reading.state).toBe<DeviceState>("awake");
      expect(reading.label).toBe("Awake");
    });

    it("is awake even when the firmware has no power contract to report", () => {
      const reading = deriveDeviceState(
        input({ reachable: true, power: null, powerSupported: false }),
      );
      expect(reading.state).toBe<DeviceState>("awake");
    });

    it("is pending while the device says it has not taken up the desired mode", () => {
      const reading = deriveDeviceState(
        input({
          reachable: true,
          power: power({ ack: "pending_wake", desired_mode: "always_on" }),
        }),
      );
      expect(reading.state).toBe<DeviceState>("pending");
      expect(reading.reason).toContain("has not");
    });

    it("is awake with an undelivered intent, because answering is the fact", () => {
      // The request is pending; the device is not. The banner in the power
      // panel carries the request's own state, and conflating the two is how
      // "awake" came to be drawn in yellow.
      const reading = deriveDeviceState(
        input({ reachable: true, power: power(), intent: intent() }),
      );
      expect(reading.state).toBe<DeviceState>("awake");
    });
  });

  describe("a device that did not answer", () => {
    it("is asleep when it is in automatic power saving and not due yet", () => {
      const reading = deriveDeviceState(input({ power: power({ mode: "auto_saver" }) }));
      expect(reading.state).toBe<DeviceState>("asleep");
      expect(reading.label).toBe("Asleep");
      expect(reading.nominal).toBe(true);
    });

    it("is uncertain with no reading at all, because silence is not evidence", () => {
      // The contract change this suite exists to pin. A failed read proves the
      // tower could not reach the panel; it proves nothing about why. With
      // nothing the device ever said to account for the silence, the honest
      // word is `uncertain` — and the confident `asleep` that used to be
      // returned here is what reported a panel woken with the BOOT button, and
      // answering other clients on the same LAN in 55 ms, as sleeping.
      const reading = deriveDeviceState(input({ power: null }));
      expect(reading.state).toBe<DeviceState>("uncertain");
      expect(reading.reason).toContain("not that it is asleep");
    });

    it("is unreachable when the last known mode was always awake", () => {
      const reading = deriveDeviceState(input({ power: power({ mode: "always_on" }) }));
      expect(reading.state).toBe<DeviceState>("unreachable");
      expect(reading.nominal).toBe(false);
    });

    it("is unreachable while an interactive window should still be open", () => {
      const reading = deriveDeviceState(
        input({
          power: power({ mode: "interactive", interactive_remaining_s: 600 }),
          // Seen a minute ago with ten minutes left: it owes us an answer.
          deviceLastSeenAt: new Date(NOW.getTime() - 60_000).toISOString(),
        }),
      );
      expect(reading.state).toBe<DeviceState>("unreachable");
    });

    it("is uncertain once that window has run out, unless the device said so", () => {
      // The tower may reasonably *expect* a device whose interactive window
      // expired to have gone back to power saving. It did not watch it happen,
      // so it does not say so: an expired window plus silence is an inference,
      // and this function only reports readings. The moment the device does
      // say something — see the case below — the word changes.
      const reading = deriveDeviceState(
        input({
          power: power({
            mode: "interactive",
            interactive_remaining_s: 300,
            sleep_intent: false,
          }),
          deviceLastSeenAt: new Date(NOW.getTime() - 30 * 60_000).toISOString(),
        }),
      );
      expect(reading.state).toBe<DeviceState>("uncertain");
    });

    it("is asleep when the device itself announced it was going to sleep", () => {
      const reading = deriveDeviceState(
        input({
          power: power({
            mode: "interactive",
            interactive_remaining_s: 300,
            sleep_intent: true,
          }),
          deviceLastSeenAt: new Date(NOW.getTime() - 30 * 60_000).toISOString(),
        }),
      );
      expect(reading.state).toBe<DeviceState>("asleep");
      expect(reading.nominal).toBe(true);
      expect(reading.reason).toContain("about to sleep");
    });

    it("is unreachable when the device is the mock, which never sleeps", () => {
      const reading = deriveDeviceState(input({ deviceMode: "mock" }));
      expect(reading.state).toBe<DeviceState>("unreachable");
      expect(reading.reason).toContain("mock");
    });

    it("is unreachable when the firmware has no hybrid contract to sleep under", () => {
      const reading = deriveDeviceState(input({ powerSupported: false }));
      expect(reading.state).toBe<DeviceState>("unreachable");
    });

    it("is uncertain when the tower has never reached it", () => {
      const reading = deriveDeviceState(
        input({ deviceLastSeenAt: null, nextWake: null, power: null }),
      );
      expect(reading.state).toBe<DeviceState>("uncertain");
      expect(reading.reason).toContain("never");
    });

    it("is uncertain once it is past due by more than the grace", () => {
      const reading = deriveDeviceState(
        input({
          nextWake: {
            at: new Date(NOW.getTime() - OVERDUE_GRACE_MS - 60_000).toISOString(),
            estimated: false,
          },
        }),
      );
      expect(reading.state).toBe<DeviceState>("uncertain");
    });

    it("is still asleep inside the grace, because a wake time is not a promise", () => {
      const reading = deriveDeviceState(
        input({
          // The device's own last word, which is what `asleep` is said on.
          power: power({ mode: "auto_saver", sleep_intent: false }),
          nextWake: {
            at: new Date(NOW.getTime() - OVERDUE_GRACE_MS + 60_000).toISOString(),
            estimated: true,
          },
        }),
      );
      expect(reading.state).toBe<DeviceState>("asleep");
    });

    it("is pending when a request is waiting and the device is merely asleep", () => {
      const reading = deriveDeviceState(input({ intent: intent() }));
      expect(reading.state).toBe<DeviceState>("pending");
      expect(reading.reason).toContain("waiting");
    });

    it("prefers the overdue reading to the pending one", () => {
      // Something is wrong with the device; that outranks a queued request.
      const reading = deriveDeviceState(
        input({
          intent: intent(),
          nextWake: {
            at: new Date(NOW.getTime() - OVERDUE_GRACE_MS - 60_000).toISOString(),
            estimated: false,
          },
        }),
      );
      expect(reading.state).toBe<DeviceState>("uncertain");
    });

    it("ignores an intent that has already been delivered", () => {
      const reading = deriveDeviceState(
        input({
          intent: intent({ appliedAt: NOW.toISOString() }),
          power: power({ mode: "auto_saver", sleep_intent: false }),
        }),
      );
      expect(reading.state).toBe<DeviceState>("asleep");
    });
  });

  it("never says offline or disconnected, on any path", () => {
    const readings = [
      deriveDeviceState(input({ reachable: true, power: power() })),
      deriveDeviceState(input()),
      deriveDeviceState(input({ intent: intent() })),
      deriveDeviceState(input({ deviceMode: "mock" })),
      deriveDeviceState(input({ deviceLastSeenAt: null, nextWake: null })),
    ];
    for (const reading of readings) {
      expect(`${reading.label} ${reading.reason}`.toLowerCase()).not.toContain("offline");
      expect(`${reading.label} ${reading.reason}`.toLowerCase()).not.toContain(
        "disconnected",
      );
    }
  });
});
