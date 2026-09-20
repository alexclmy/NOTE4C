/**
 * What a failed read is allowed to claim about the panel, and what it is not.
 *
 * THE SEQUENCE THIS FILE REPRODUCES
 * ---------------------------------
 * From the production install, on 2026-09-17, with timestamps taken from the
 * push ledger and from the device itself:
 *
 *   14:36:28Z  the tower reads the panel successfully. It is in an interactive
 *              window, woken by the BOOT button.
 *   14:49:02Z  a send is queued. The read behind it fails in **0 ms** with
 *              EHOSTUNREACH — the host's own routing table answering, not the
 *              network — and that sentence is written to the ledger.
 *   14:54Z     the Device page shows the badge `Asleep`, the sentence "Not
 *              answering, which is the designed state between refreshes", and,
 *              directly underneath, a red UNREACHABLE banner. Two contradictory
 *              claims from one failed read.
 *   15:16Z     `GET /api/v1/dashboard/status` at 192.168.0.60, from this same
 *              machine, answers HTTP 200 in 55 ms: `awake: true`, `mode:
 *              interactive`, `last_wake_reason: "button"`.
 *
 * The panel was awake for all of it. Three separate defects produced that
 * screen, and each has a group below:
 *
 *  1. the status route dropped the power block on a failed read, so the state
 *     function had no evidence and fell through to `asleep`;
 *  2. a sub-millisecond routing refusal — this machine declining to try, for
 *     twenty seconds, after one failed address resolution — was classified and
 *     worded as though the network had answered;
 *  3. nothing ordered the concurrent reads, so a slow one could land after a
 *     fast one and put the stale answer back.
 */

import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import {
  DeviceError,
  STATUS_RETRY_BACKOFF_MS,
  MANUAL_STATUS_RETRY_BUDGET_MS,
  type DeviceTransportDiagnostics,
} from "@/server/device/client";
import { describeDeviceFailure } from "@/server/device/failure";
import {
  DeviceTransportError,
  LOCAL_VERDICT_MAX_MS,
} from "@/server/device/transport";
import {
  persistConfirmedReading,
  readLastConfirmed,
  recordConfirmedReading,
  resetConfirmedReadingForTests,
} from "@/server/device/lastConfirmed";
import { readState } from "@/server/store/state";
import {
  deriveDeviceState,
  type ConfirmedReading,
  type DevicePower,
  type DeviceState,
} from "@/core/power";
import {
  publishDeviceStatus,
  resetDeviceReadingForTests,
  toDeviceReading,
} from "@/ui/useDeviceState";
import { useTempDataRoot } from "./helpers/tempRoot";

/**
 * The panel's own answer at 15:16:05Z, copied from the live read. It is the
 * ground truth the interface was contradicting.
 */
function awakeAfterButton(overrides: Partial<DevicePower> = {}): DevicePower {
  return {
    contract: 1,
    mode: "interactive",
    desired_mode: "interactive",
    ack: "acknowledged",
    awake: true,
    sleep_intent: false,
    interactive_remaining_s: 900,
    wake_interval_min: 60,
    timer_armed: false,
    next_wake_in_s: 0,
    next_wake_epoch: null,
    last_wake_reason: "button",
    last_outcome: null,
    budget_exhausted_phase: null,
    consecutive_failures: 0,
    battery: { present: true, calibrated: true, plausible: true, mv: 3937, percent: 80 },
    charge: { state: "no_power", charging: false },
    ...overrides,
  };
}

/** The failure the captures show, exactly: connect phase, EHOSTUNREACH, 0 ms. */
function holdDownDiagnostics(): DeviceTransportDiagnostics {
  return {
    fault: "network",
    phase: "connect",
    errno: "EHOSTUNREACH",
    syscall: "connect",
    requestSent: false,
    connected: false,
    answered: false,
    heldLocally: true,
    endpoint: "192.168.0.60:80",
    elapsedMs: 0,
  };
}

// ------------------------------------------------ 1. the word on the badge --

describe("the sequence that put Asleep on an awake panel", () => {
  const seenAt = new Date("2026-09-17T14:36:28.180Z");
  const clicked = new Date("2026-09-17T14:54:00.000Z");

  /** What the status route now returns for the failed read at 14:54Z. */
  const afterFailedRead = {
    reachable: false,
    deviceMode: "real" as const,
    powerSupported: null,
    power: null,
    powerIntent: null,
    deviceLastSeenAt: seenAt.toISOString(),
    nextWake: {
      at: new Date(seenAt.getTime() + 60 * 60_000).toISOString(),
      estimated: true,
    },
    lastConfirmed: {
      at: seenAt.toISOString(),
      power: awakeAfterButton(),
      powerSupported: true,
    } satisfies ConfirmedReading,
    readAt: clicked.toISOString(),
    detail:
      "This machine refused the connection to 192.168.0.60:80 from its own routing table, in 0 ms (EHOSTUNREACH)",
  };

  it("never says Asleep on the strength of a failed read", () => {
    const reading = deriveDeviceState({
      ...toDeviceReading(afterFailedRead).input,
      now: clicked,
    });
    expect(reading.state).not.toBe<DeviceState>("asleep");
  });

  it("says Unreachable, and says when the device was last confirmed awake", () => {
    // The interactive window the panel reported at 14:36 had 900 s in it, so at
    // 14:54 it had run out — but the tower never watched that happen. What it
    // has is one reading, and it reports it with its age rather than turning it
    // into a claim about now.
    const early = deriveDeviceState({
      ...toDeviceReading(afterFailedRead).input,
      now: new Date(seenAt.getTime() + 5 * 60_000),
    });
    expect(early.state).toBe<DeviceState>("unreachable");
    expect(early.reason).toContain("interactive window was still open");
    expect(early.reason).toContain("It last answered at");
    expect(early.reason).toContain("5 minutes ago");
    expect(early.nominal).toBe(false);
  });

  it("falls back to uncertain, not asleep, once that evidence has expired", () => {
    const reading = deriveDeviceState({
      ...toDeviceReading(afterFailedRead).input,
      now: clicked,
    });
    expect(reading.state).toBe<DeviceState>("uncertain");
    expect(reading.reason).toContain("not that it is asleep");
    expect(reading.reason).toContain("It last answered at");
  });

  it("becomes Awake the moment a read succeeds, and drops the stale error", () => {
    resetDeviceReadingForTests();
    // The failed read is on screen, with its banner.
    expect(publishDeviceStatus(afterFailedRead)).toBe(true);

    // "Check now", and this time the panel answers exactly as it did live.
    const answeredAt = new Date("2026-09-17T15:16:05.643Z");
    const published = publishDeviceStatus({
      reachable: true,
      deviceMode: "real",
      power: awakeAfterButton(),
      powerSupported: true,
      powerIntent: null,
      deviceLastSeenAt: answeredAt.toISOString(),
      nextWake: null,
      lastConfirmed: {
        at: answeredAt.toISOString(),
        power: awakeAfterButton(),
        powerSupported: true,
      },
      readAt: answeredAt.toISOString(),
    });
    expect(published).toBe(true);

    const reading = toDeviceReading({
      reachable: true,
      deviceMode: "real",
      power: awakeAfterButton(),
      powerSupported: true,
      readAt: answeredAt.toISOString(),
    });
    const state = deriveDeviceState({ ...reading.input, now: answeredAt });
    expect(state.state).toBe<DeviceState>("awake");
    expect(state.nominal).toBe(true);
    // Nothing in the new reading carries the old sentence forward: the payload
    // that held it was replaced whole.
    expect(JSON.stringify(reading)).not.toContain("EHOSTUNREACH");
  });

  it("says Asleep when the device itself reports it is not awake", () => {
    const at = new Date("2026-09-17T15:20:00.000Z");
    const state = deriveDeviceState({
      ...toDeviceReading({
        reachable: true,
        deviceMode: "real",
        power: awakeAfterButton({ awake: false, sleep_intent: true }),
        powerSupported: true,
      }).input,
      now: at,
    });
    expect(state.state).toBe<DeviceState>("asleep");
  });

  it("does not read auto_saver as asleep while the device says it is awake", () => {
    // Stated in the brief and worth its own case: the mode is a policy, not a
    // state. A panel in automatic power saving that answers is awake.
    const state = deriveDeviceState({
      ...toDeviceReading({
        reachable: true,
        deviceMode: "real",
        power: awakeAfterButton({ mode: "auto_saver", awake: true }),
        powerSupported: true,
      }).input,
    });
    expect(state.state).toBe<DeviceState>("awake");
  });
});

// ------------------------------------- 2. a refusal this machine made itself --

describe("a connection this machine refused out of its own routing table", () => {
  it("is recognised by the timing no network failure can have", () => {
    const held = new DeviceTransportError("connect EHOSTUNREACH 192.168.0.60:80", {
      phase: "connect",
      errno: "EHOSTUNREACH",
      syscall: "connect",
      requestSent: false,
      elapsedMs: 0,
    });
    expect(held.heldLocally).toBe(true);

    // Measured on the production host: a genuine resolution of a neighbour that
    // is not there costs seconds, because ARP is actually tried. Only the
    // cached verdict comes back instantly.
    const genuine = new DeviceTransportError("connect EHOSTUNREACH", {
      phase: "connect",
      errno: "EHOSTUNREACH",
      requestSent: false,
      elapsedMs: LOCAL_VERDICT_MAX_MS + 1,
    });
    expect(genuine.heldLocally).toBe(false);
  });

  it("is not claimed for a host that answered, however fast it answered", () => {
    const refused = new DeviceTransportError("connect ECONNREFUSED", {
      phase: "connect",
      errno: "ECONNREFUSED",
      requestSent: false,
      elapsedMs: 0,
    });
    expect(refused.heldLocally).toBe(false);
  });

  it("is a fault of this machine, not a panel that is away", () => {
    const failure = describeDeviceFailure(
      new DeviceError("unreachable", "held", null, true, holdDownDiagnostics()),
    );
    expect(failure.kind).toBe("transport");
    expect(failure.heldLocally).toBe(true);
    expect(failure.deviceAnswered).toBe(false);
    // Notable: a reading taken during a hold-down is worthless, and a person
    // looking at a queued frame deserves to know the tower never asked.
    expect(failure.notable).toBe(true);
  });

  it("leaves an ordinary silence classified exactly as before", () => {
    const failure = describeDeviceFailure(
      new DeviceError("unreachable", "quiet", null, true, {
        ...holdDownDiagnostics(),
        errno: "EHOSTUNREACH",
        elapsedMs: 5_000,
        heldLocally: false,
      }),
    );
    expect(failure.kind).toBe("absent");
    expect(failure.notable).toBe(false);
    expect(failure.heldLocally).toBe(false);
  });
});

// ------------------------------------------- 3. what a status read may retry --

describe("the read-only retry budget", () => {
  it("is long enough to outlast the hold-down it exists for", () => {
    // net.link.ether.inet.host_down_time on the production host, in ms.
    const KERNEL_HOLD_DOWN_MS = 20_000;
    const total = STATUS_RETRY_BACKOFF_MS.reduce((sum, ms) => sum + ms, 0);
    expect(total).toBeGreaterThanOrEqual(KERNEL_HOLD_DOWN_MS);
    expect(MANUAL_STATUS_RETRY_BUDGET_MS).toBeGreaterThanOrEqual(KERNEL_HOLD_DOWN_MS);
  });

  it("is spent only on status, never on a write", () => {
    // Structural, and the property is the product's first rule: exactly one
    // outbound write per intent, ever. `retryBudgetMs` must not reach `putFrame`,
    // `patchConfig` or `runAction`.
    const source = readFileSync(
      resolve(__dirname, "../../src/server/device/client.ts"),
      "utf8",
    );
    const retryLoops = source.match(/retryBudgetMs/g) ?? [];
    expect(retryLoops.length).toBeGreaterThan(0);
    const afterStatus = source.slice(source.indexOf("async putFrame"));
    expect(afterStatus).not.toContain("retryBudgetMs");
    expect(afterStatus).not.toContain("STATUS_RETRY_BACKOFF_MS");
  });
});

// ------------------------------------------------ 4. two reads at once --

describe("two reads in flight at once", () => {
  beforeEach(() => {
    resetDeviceReadingForTests();
  });

  const base = {
    reachable: true,
    deviceMode: "real" as const,
    powerSupported: true,
    power: awakeAfterButton(),
  };

  it("keeps the newer answer when an older one lands after it", () => {
    const manual = publishDeviceStatus({
      ...base,
      readAt: "2026-09-17T15:16:05.000Z",
    });
    expect(manual).toBe(true);

    // The poll that started before the click and finished after it. Under the
    // defect this is what put the device back to "not answering" a second after
    // it had been proven awake.
    const stalePoll = publishDeviceStatus({
      ...base,
      reachable: false,
      power: null,
      readAt: "2026-09-17T15:15:40.000Z",
    });
    expect(stalePoll).toBe(false);
  });

  it("accepts a genuinely newer answer, including a failure", () => {
    publishDeviceStatus({ ...base, readAt: "2026-09-17T15:16:05.000Z" });
    const later = publishDeviceStatus({
      ...base,
      reachable: false,
      power: null,
      readAt: "2026-09-17T15:17:00.000Z",
    });
    expect(later).toBe(true);
  });
});

// ------------------------------------- 5. remembering what the device said --

describe("the last confirmed reading", () => {
  let temp: ReturnType<typeof useTempDataRoot>;

  beforeEach(() => {
    temp = useTempDataRoot();
    resetConfirmedReadingForTests();
  });

  afterEach(() => {
    temp.dispose();
  });

  const reading: ConfirmedReading = {
    at: "2026-09-17T14:36:28.180Z",
    power: awakeAfterButton(),
    powerSupported: true,
  };

  it("survives a restart, with the timestamp the device was read at", () => {
    persistConfirmedReading(reading);
    resetConfirmedReadingForTests();
    const stored = readLastConfirmed();
    expect(stored?.at).toBe(reading.at);
    expect(stored?.power?.mode).toBe("interactive");
    expect(readState().deviceLastSeenAt).toBe(reading.at);
  });

  it("does not move the document version, so a rollback can still read it", () => {
    // The error log on the production install is full of `schema_version 3 is
    // newer than this build understands (2)`, from the last time a state
    // document outran a binary. An additive optional field cannot repeat it.
    persistConfirmedReading(reading);
    const raw = JSON.parse(
      readFileSync(resolve(temp.root, "state.json"), "utf8"),
    ) as { schema_version: number; lastDeviceReading?: unknown };
    expect(raw.schema_version).toBe(3);
    expect(raw.lastDeviceReading).not.toBeUndefined();
  });

  it("reads a state file that has never carried one", () => {
    expect(readLastConfirmed()).toBeNull();
    expect(readState().lastDeviceReading).toBeNull();
  });

  it("never lets a reading it cannot parse take the whole document down", () => {
    // This field embeds a device's schema in the tower's own state, and the
    // device is the one part of this system that can change without this
    // repository being rebuilt. A power block from a firmware this build does
    // not understand degrades to "no reading"; it does not corrupt the
    // document that also holds the selected dashboard and the device address.
    persistConfirmedReading(reading);
    const file = resolve(temp.root, "state.json");
    const raw = JSON.parse(readFileSync(file, "utf8")) as Record<string, unknown>;
    raw.lastDeviceReading = { at: 12345, power: { mode: "hibernate" } };
    writeFileSync(file, JSON.stringify(raw));
    resetConfirmedReadingForTests();

    const state = readState();
    expect(state.lastDeviceReading).toBeNull();
    expect(state.deviceAddress).toBe(readState().deviceAddress);
    expect(state.schema_version).toBe(3);
    expect(readLastConfirmed()).toBeNull();
  });

  it("prefers whichever of memory and disk is newer", () => {
    persistConfirmedReading(reading);
    const newer: ConfirmedReading = {
      at: "2026-09-17T15:16:05.643Z",
      power: awakeAfterButton({ interactive_remaining_s: 76 }),
      powerSupported: true,
    };
    recordConfirmedReading(newer);
    expect(readLastConfirmed()?.at).toBe(newer.at);

    // And an answer that arrives out of order never displaces a newer one.
    recordConfirmedReading(reading);
    expect(readLastConfirmed()?.at).toBe(newer.at);
  });
});

// ------------------------------------------------ 6. the route's own shape --

describe("GET /api/device/status", () => {
  const source = readFileSync(
    resolve(__dirname, "../../app/api/device/status/route.ts"),
    "utf8",
  );
  const code = source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/.*$/gm, "");

  it("answers with the last confirmed reading on both paths", () => {
    expect(code.match(/lastConfirmed/g)?.length).toBeGreaterThanOrEqual(2);
  });

  it("is still a read: it notes the answer in memory and writes no state", () => {
    expect(code).toContain("recordConfirmedReading");
    expect(code).not.toContain("persistConfirmedReading");
    expect(code).not.toContain("updateState");
  });

  it("refuses to be cached, and spends the longer budget only when asked", () => {
    expect(code).toContain("no-store");
    expect(code).toContain("MANUAL_STATUS_RETRY_BUDGET_MS");
    expect(code).toContain('params.get("force")');
  });

  it("answers `observe=0` before it builds a client, and opens no socket", () => {
    // The first paint of the Device page. It has to be ahead of
    // `deviceContext()` — which in mock mode starts a panel — and it must not
    // reach `client.status()` at all, or it is the blocking read again under a
    // new name.
    const observeAt = code.indexOf('params.get("observe") === "0"');
    expect(observeAt).toBeGreaterThan(-1);
    expect(observeAt).toBeLessThan(code.indexOf("await deviceContext()"));
    expect(code.slice(observeAt, code.indexOf("await deviceContext()"))).not.toContain(
      "client.status(",
    );
  });
});
