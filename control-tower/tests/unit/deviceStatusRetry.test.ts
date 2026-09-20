/**
 * What `DeviceClient.status()` retries, and — much more importantly — what it
 * does not.
 *
 * The licence is narrow and the file exists to keep it narrow. `GET
 * /api/v1/dashboard/status` changes nothing on the panel, so reading it twice
 * is free; every write in this client is exactly one attempt per intent and
 * must stay that way, which is why `putFrame` has no budget to pass.
 *
 * The retry is not a general "try again on error" either. It is aimed at one
 * measured failure mode: after a single failed address resolution the host
 * refuses every connection to that address, out of its own table, in about a
 * millisecond, for twenty seconds. Waiting is the whole remedy, and nothing
 * else here benefits from it — a panel that genuinely is not there fails
 * slowly, because ARP is actually attempted, and is returned at once.
 *
 * The transport is replaced wholesale so the sequence can be scripted; the
 * real socket behaviour is pinned in deviceTransport.test.ts.
 */

import { beforeEach, describe, expect, it, vi } from "vitest";

const attempts: string[] = [];
let script: (attempt: number) => unknown = () => {
  throw new Error("no script");
};

vi.mock("@/server/device/transport", async (importOriginal) => {
  const actual = await importOriginal<typeof import("@/server/device/transport")>();
  return {
    ...actual,
    deviceRequest: (request: { path: string }) => {
      attempts.push(request.path);
      return Promise.resolve().then(() => script(attempts.length));
    },
  };
});

const { DeviceClient, DeviceError, DeviceUncertainError } = await import(
  "@/server/device/client"
);
const { DeviceTransportError, DeviceTransportTimeout } = await import(
  "@/server/device/transport"
);

const TOKEN = "d".repeat(64);

function client(): InstanceType<typeof DeviceClient> {
  return new DeviceClient({ mode: "real", address: "192.168.0.60", token: TOKEN });
}

/** The production failure: connect phase, EHOSTUNREACH, no packet sent, 0 ms. */
function heldLocally(): never {
  throw new DeviceTransportError("connect EHOSTUNREACH 192.168.0.60:80", {
    phase: "connect",
    errno: "EHOSTUNREACH",
    syscall: "connect",
    requestSent: false,
    elapsedMs: 0,
  });
}

/** A panel that is genuinely not there: the resolution was actually attempted. */
function genuinelyAbsent(): never {
  throw new DeviceTransportError("connect EHOSTUNREACH 192.168.0.60:80", {
    phase: "connect",
    errno: "EHOSTUNREACH",
    syscall: "connect",
    requestSent: false,
    elapsedMs: 5_000,
  });
}

function answered(): { status: number; headers: Headers; body: Uint8Array; elapsedMs: number } {
  const body = {
    firmware: "Marvin 0.2",
    api: 2,
    capabilities: ["power.hybrid.v1"],
    provisioned: true,
    lockdown: false,
    stored: { present: true, seq: 110, sha256: "a".repeat(64) },
    displayed: { present: true, seq: 110, sha256: "a".repeat(64) },
    refresh: { state: "idle", pending: false, renders: 3, skipped: 0, coalesced: 0 },
    power: {
      contract: 1,
      mode: "interactive",
      desired_mode: "interactive",
      ack: "acknowledged",
      awake: true,
      sleep_intent: false,
      interactive_remaining_s: 76,
      wake_interval_min: 60,
      timer_armed: false,
      next_wake_in_s: 0,
      next_wake_epoch: null,
      last_wake_reason: "button",
      last_outcome: null,
      consecutive_failures: 0,
      battery: { present: true, calibrated: true, plausible: true, mv: 3937, percent: 80 },
      charge: { state: "no_power", charging: false },
    },
  };
  return {
    status: 200,
    headers: new Headers({ "content-type": "application/json" }),
    body: new TextEncoder().encode(JSON.stringify(body)),
    elapsedMs: 55,
  };
}

describe("status reads and the hold-down", () => {
  beforeEach(() => {
    attempts.length = 0;
  });

  it("waits out a hold-down and returns the panel's real answer", async () => {
    // Exactly the production shape: the click lands inside the window, the
    // first reads are refused locally, and the panel was awake the whole time.
    script = (n) => (n < 3 ? heldLocally() : answered());
    const status = await client().status({ retryBudgetMs: 1_000 });
    expect(attempts).toHaveLength(3);
    expect(status.power?.awake).toBe(true);
    expect(status.power?.last_wake_reason).toBe("button");
  });

  it("gives up inside its budget rather than retrying for ever", async () => {
    script = () => heldLocally();
    await expect(client().status({ retryBudgetMs: 1_000 })).rejects.toBeInstanceOf(
      DeviceError,
    );
    // 150 ms + 850 ms of waiting fits; a third wait does not.
    expect(attempts).toHaveLength(3);
  });

  it("does not spend the budget on a panel that genuinely did not answer", async () => {
    script = () => genuinelyAbsent();
    await expect(client().status({ retryBudgetMs: 21_000 })).rejects.toBeInstanceOf(
      DeviceError,
    );
    expect(attempts).toHaveLength(1);
  });

  it("does not retry a deadline, which is the uncertain case", async () => {
    script = () => {
      throw new DeviceTransportTimeout({
        phase: "response",
        requestSent: true,
        timeoutMs: 10_000,
      });
    };
    await expect(client().status({ retryBudgetMs: 21_000 })).rejects.toBeInstanceOf(
      DeviceUncertainError,
    );
    expect(attempts).toHaveLength(1);
  });

  it("does not retry a device that answered and said no", async () => {
    script = () => ({
      status: 503,
      headers: new Headers(),
      body: new TextEncoder().encode('{"error":"no_runner"}'),
      elapsedMs: 12,
    });
    await expect(client().status({ retryBudgetMs: 21_000 })).rejects.toBeInstanceOf(
      DeviceError,
    );
    expect(attempts).toHaveLength(1);
  });

  it("keeps the background default at one extra attempt", async () => {
    script = () => heldLocally();
    await expect(client().status()).rejects.toBeInstanceOf(DeviceError);
    expect(attempts).toHaveLength(2);
  });

  it("carries the diagnosis through to the failure, for the page to read", async () => {
    script = () => heldLocally();
    const error = await client()
      .status()
      .then(
        () => null,
        (caught: unknown) => caught as InstanceType<typeof DeviceError>,
      );
    expect(error).toBeInstanceOf(DeviceError);
    if (error === null) return;
    expect(error.transport?.heldLocally).toBe(true);
    expect(error.transport?.requestSent).toBe(false);
    // The sentence a person sees. It must not claim anything about the panel.
    expect(error.message).toContain("own routing table");
    expect(error.message).toContain("nothing was learned about the panel");
    expect(error.message).not.toContain("accepted a connection");
  });

  it("never retries a frame, whatever the failure was", async () => {
    script = () => heldLocally();
    const frame = new Uint8Array(30_000);
    await expect(
      client().putFrame(frame, { sha256: "b".repeat(64), idempotencyKey: "k" }),
    ).rejects.toBeInstanceOf(DeviceError);
    expect(attempts).toHaveLength(1);
  });
});
