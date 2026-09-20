/**
 * What a page is allowed to draw before the device has been asked.
 *
 * THE DEFECT THIS FILE REPRODUCES
 * ------------------------------
 * `/overview` and `/device` held their skeletons — "Reading the device and the
 * sources." / "Reading the device." — for as long as the panel took to answer,
 * which on the production install was long enough to look broken. Measured
 * against the in-repo mock with a socket that is accepted and never answered
 * (`MockDevice.stallMs`, which is what a dropped packet or a `node` binary
 * without macOS Local Network permission actually looks like), at 3 000 ms per
 * read:
 *
 *   /device    shell status 3 033 ms → page status 6 044 ms → config 9 051 ms
 *   /overview  shell status 3 043 ms → forced snapshot 6 052 ms
 *
 * Three device reads and two device reads, *consecutive* rather than
 * concurrent, because every device call in this process goes through one
 * mutex. At the real ten-second transport timeout that is thirty seconds and
 * twenty seconds of blank page. Nothing on either page needed it: the device
 * block is one card, and the rest is local files.
 *
 * THE RULE
 * --------
 * A page paints from what the tower already knows, and the read that
 * establishes what is true *now* runs behind it. The cost is one new fact —
 * whether anything was actually asked — and the whole of this file is about
 * that fact being carried honestly, because the alternative is the failure
 * mode PR #6 corrected arriving from the other direction: a red UNREACHABLE,
 * or a confident "Asleep", asserted about a read that never happened.
 */

import { beforeEach, describe, expect, it } from "vitest";
import {
  deriveDeviceState,
  type DevicePower,
  type DeviceStateInput,
} from "@/core/power";
import { readLastKnownDevice } from "@/server/device/lastKnown";
import {
  persistConfirmedReading,
  resetConfirmedReadingForTests,
} from "@/server/device/lastConfirmed";
import {
  publishDeviceStatus,
  resetDeviceReadingForTests,
  toDeviceReading,
} from "@/ui/useDeviceState";
import { useTempDataRoot } from "./helpers/tempRoot";

const NOW = new Date("2026-09-17T16:45:00.000Z");

function power(overrides: Partial<DevicePower> = {}): DevicePower {
  return {
    contract: 1,
    mode: "auto_saver",
    desired_mode: "auto_saver",
    ack: "acknowledged",
    awake: true,
    sleep_intent: false,
    interactive_remaining_s: 0,
    wake_interval_min: 60,
    timer_armed: true,
    next_wake_in_s: 1_800,
    next_wake_epoch: null,
    last_wake_reason: "timer",
    last_outcome: null,
    budget_exhausted_phase: null,
    consecutive_failures: 0,
    battery: { present: true, calibrated: true, plausible: true, mv: 3937, percent: 78 },
    charge: { state: "no_power", charging: false },
    ...overrides,
  };
}

function input(overrides: Partial<DeviceStateInput> = {}): DeviceStateInput {
  return {
    reachable: false,
    observed: false,
    power: null,
    powerSupported: null,
    intent: null,
    deviceMode: "real",
    nextWake: null,
    deviceLastSeenAt: null,
    lastConfirmed: null,
    now: NOW,
    ...overrides,
  };
}

describe("a read that never happened claims nothing", () => {
  it("never says unreachable, even when a read would have", () => {
    // `always_on` is the strongest case for UNREACHABLE there is: the device
    // said it would stay awake and it is not answering. But nobody asked, so
    // there is no silence to interpret — and this is exactly the panel from
    // the incident, which was awake and answering the whole time the interface
    // said otherwise.
    const evidence = {
      at: "2026-09-17T16:42:53.366Z",
      power: power({ mode: "always_on", timer_armed: false }),
      powerSupported: true,
    };
    const asked = deriveDeviceState(input({ observed: true, lastConfirmed: evidence }));
    expect(asked.state).toBe("unreachable");

    const notAsked = deriveDeviceState(input({ lastConfirmed: evidence }));
    expect(notAsked.state).toBe("uncertain");
    expect(notAsked.reason).toContain("not read the device yet");
  });

  it("never says unreachable about the mock either", () => {
    // The mock does not sleep, so silence from it is a fault — but only once
    // somebody has listened for it.
    expect(deriveDeviceState(input({ observed: true, deviceMode: "mock" })).state).toBe(
      "unreachable",
    );
    expect(deriveDeviceState(input({ deviceMode: "mock" })).state).toBe("uncertain");
  });

  it("still says asleep on the device's own account of itself", () => {
    // A sleeping panel is the ordinary case and must not need a network round
    // trip to look ordinary: this is the state the page paints instantly.
    const reading = deriveDeviceState(
      input({
        lastConfirmed: {
          at: "2026-09-17T16:00:00.000Z",
          power: power({ mode: "auto_saver", timer_armed: true }),
          powerSupported: true,
        },
      }),
    );
    expect(reading.state).toBe("asleep");
    expect(reading.nominal).toBe(true);
  });

  it("says uncertain, and says why, when it has nothing to go on", () => {
    const reading = deriveDeviceState(input());
    expect(reading.state).toBe("uncertain");
    expect(reading.reason).toContain("has not read the device yet");
  });

  it("still reports a waiting request, which is the tower's own fact", () => {
    const reading = deriveDeviceState(
      input({
        intent: {
          mode: "always_on",
          interactiveMinutes: null,
          wakeIntervalMinutes: null,
          requestedAt: NOW.toISOString(),
          appliedAt: null,
          lastAttemptAt: null,
          lastAttemptError: null,
          attempts: 0,
        },
      }),
    );
    expect(reading.state).toBe("pending");
  });

  it("is replaced by awake the moment a read actually lands", () => {
    // The requirement in the brief, stated as the assertion it is: a
    // successful `awake: true` read replaces `Uncertain`.
    const before = deriveDeviceState(input());
    expect(before.state).toBe("uncertain");

    const after = deriveDeviceState(
      input({ observed: true, reachable: true, power: power({ awake: true }) }),
    );
    expect(after.state).toBe("awake");
    expect(after.nominal).toBe(true);
  });

  it("defaults to observed, so every existing caller is unchanged", () => {
    const legacy = input({ observed: true, deviceMode: "mock" });
    delete (legacy as { observed?: boolean }).observed;
    expect(deriveDeviceState(legacy).state).toBe("unreachable");
  });
});

describe("the tower's record, read without opening a socket", () => {
  let temp: ReturnType<typeof useTempDataRoot>;

  beforeEach(() => {
    temp = useTempDataRoot();
    resetConfirmedReadingForTests();
    return () => temp.dispose();
  });

  it("carries the last confirmed reading and marks itself unobserved", () => {
    persistConfirmedReading({
      at: "2026-09-17T16:42:53.366Z",
      power: power({ mode: "interactive", interactive_remaining_s: 878 }),
      powerSupported: true,
    });

    const known = readLastKnownDevice(NOW);
    expect(known.observed).toBe(false);
    expect(known.reachable).toBe(false);
    expect(known.lastConfirmed?.at).toBe("2026-09-17T16:42:53.366Z");
    // Not "Not answering." — nothing was asked, so nothing failed to answer.
    expect(known.reachabilityNote).toContain("Not read yet");
    expect(known.reachabilityNote).not.toContain("Not answering");
  });

  it("says nothing when the tower has never heard from the device", () => {
    const known = readLastKnownDevice(NOW);
    expect(known.lastConfirmed).toBeNull();
    expect(deriveDeviceState(toDeviceReading(known).input).state).toBe("uncertain");
  });
});

describe("a reading nobody took never displaces one somebody did", () => {
  beforeEach(() => {
    resetDeviceReadingForTests();
  });

  it("refuses to overwrite an observed reading with the tower's record", () => {
    // The race the ordering exists for, arriving from the other direction: the
    // Device page's real read lands, then the shell's first-paint read — which
    // has no observation time to be ordered by — comes home behind it.
    expect(
      publishDeviceStatus({
        reachable: true,
        observed: true,
        deviceMode: "real",
        power: power({ awake: true, mode: "always_on" }),
        powerSupported: true,
        readAt: "2026-09-17T16:45:00.000Z",
      }),
    ).toBe(true);

    expect(
      publishDeviceStatus({
        reachable: false,
        observed: false,
        deviceMode: "real",
        lastConfirmed: {
          at: "2026-09-17T16:00:00.000Z",
          power: power(),
          powerSupported: true,
        },
      }),
    ).toBe(false);
  });

  it("accepts an observed reading over the record, whichever way round", () => {
    expect(
      publishDeviceStatus({ reachable: false, observed: false, deviceMode: "real" }),
    ).toBe(true);
    expect(
      publishDeviceStatus({
        reachable: true,
        observed: true,
        deviceMode: "real",
        power: power({ awake: true }),
        readAt: "2026-09-17T16:45:00.000Z",
      }),
    ).toBe(true);
  });

  it("still orders two observed readings by the server's clock", () => {
    // Unchanged from PR #6, and re-pinned here because the new rule sits
    // directly above it: a slow read that lands late must not win.
    publishDeviceStatus({
      reachable: true,
      observed: true,
      deviceMode: "real",
      power: power({ awake: true }),
      readAt: "2026-09-17T16:45:10.000Z",
    });
    expect(
      publishDeviceStatus({
        reachable: false,
        observed: true,
        deviceMode: "real",
        readAt: "2026-09-17T16:45:00.000Z",
      }),
    ).toBe(false);
  });
});
