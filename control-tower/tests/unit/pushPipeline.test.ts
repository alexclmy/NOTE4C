import { afterEach, beforeEach, describe, expect, it } from "vitest";
import fs from "node:fs";
import { createHash } from "node:crypto";
import { MockDevice } from "@mock/server";
import { DeviceClient } from "@/server/device/client";
import {
  acknowledge,
  deliverQueuedPush,
  enqueueFrame,
  prepareFrame,
  pollSchedule,
  push,
  readQueuedFrame,
  recheck,
  WORST_CASE_ACK_MS,
} from "@/server/device/pushPipeline";
import {
  blockingPush,
  findPush,
  lastVerifiedPush,
  queuedPush,
  readLedger,
  readPushes,
  resolveSha,
} from "@/server/device/ledger";
import {
  MAX_QUEUED_PUSH_ATTEMPTS,
  QUEUED_PUSH_MAX_AGE_MS,
} from "@/core/pushQueue";
import { readAudit, redactParams } from "@/server/audit";
import { moduleDefinition } from "@/core/render/modules";
import { paths } from "@/server/store/paths";
import { starterDashboard, type DashboardDoc } from "@/core/model";
import { fixtureSources, FIXTURE_NOW } from "./fixtures/render";
import { useTempDataRoot } from "./helpers/tempRoot";

const TOKEN = "c".repeat(64);

let temp: ReturnType<typeof useTempDataRoot>;
let device: MockDevice;
let client: DeviceClient;

/** No real waiting: the schedule is exercised, the wall clock is not. */
const instantSleep = async (): Promise<void> => {
  await new Promise((resolve) => setImmediate(resolve));
};

function doc(title = "Kitchen panel"): DashboardDoc {
  const d = starterDashboard(title, FIXTURE_NOW);
  d.id = "d_test";
  return d;
}

async function makeClient(options: {
  panelDelayMs?: number;
  failAck?: boolean;
  token?: string | null;
} = {}): Promise<void> {
  device = new MockDevice({
    token: options.token === undefined ? TOKEN : options.token,
    panelDelayMs: options.panelDelayMs ?? 5,
    failAck: options.failAck ?? false,
  });
  const origin = await device.listen(0);
  client = new DeviceClient({
    mode: "mock",
    address: "192.168.7.7",
    mockOrigin: origin,
    token: TOKEN,
    timeoutMs: 2_000,
  });
}

/** Give the mock's panel timer a chance to fire between polls. */
const pacedSleep = async (): Promise<void> => {
  await new Promise((resolve) => setTimeout(resolve, 25));
};

beforeEach(async () => {
  temp = useTempDataRoot();
  await makeClient();
});

afterEach(async () => {
  await device.close();
  temp.dispose();
});

describe("poll schedule", () => {
  it("starts fast, then covers the firmware worst case", () => {
    const schedule = pollSchedule();
    expect(schedule.slice(0, 3)).toEqual([2_000, 5_000, 10_000]);
    expect(schedule).toContain(90_000);
    expect(schedule.at(-1)).toBe(WORST_CASE_ACK_MS);
    expect(WORST_CASE_ACK_MS).toBe(390_000);
    // Strictly increasing, so a poll never runs backwards.
    for (let i = 1; i < schedule.length; i += 1) {
      expect(schedule[i] as number).toBeGreaterThan(schedule[i - 1] as number);
    }
  });
});

describe("prepareFrame", () => {
  it("produces 30000 bytes and two distinct digests", () => {
    const prepared = prepareFrame(doc(), fixtureSources(), FIXTURE_NOW);
    expect(prepared.bytes.length).toBe(30_000);
    expect(prepared.sha256).toMatch(/^[0-9a-f]{64}$/);
    expect(prepared.semanticHash).toMatch(/^[0-9a-f]{64}$/);
    // The frame carries a live timestamp; the semantic frame does not.
    expect(prepared.semanticHash).not.toBe(prepared.sha256);
  });
});

describe("happy path", () => {
  it("goes pending, sent, verified_displayed", async () => {
    const result = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });

    expect(result.outcome).toBe("verified_displayed");
    expect(result.seq).toBe(1);
    expect(result.deduped).toBe(false);
    expect(result.panelMs).not.toBeNull();

    const states = readLedger().map((line) => line.state);
    expect(states).toEqual(["pending", "sent", "verified_displayed"]);

    const record = readPushes()[0];
    expect(record?.state).toBe("verified_displayed");
    expect(record?.first.sha256).toBe(result.sha256);
    expect(blockingPush()).toBeNull();
  });

  it("records the digest so it can be resolved back to a version", async () => {
    const result = await push({
      doc: doc(),
      version: 4,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    expect(resolveSha(result.sha256)).toEqual({
      dashboardId: "d_test",
      dashboardTitle: "Kitchen panel",
      version: 4,
    });
    expect(resolveSha("f".repeat(64))).toBeNull();
    expect(resolveSha("")).toBeNull();
  });

  it("writes an audit entry that says the device confirmed", async () => {
    await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    const entries = readAudit();
    expect(entries[0]).toMatchObject({
      action: "device.push",
      outcome: "ok",
      deviceConfirmed: true,
    });
  });
});

describe("semantic dedup gate", () => {
  it("refuses an unchanged push and offers the cost honestly", async () => {
    const first = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    expect(first.outcome).toBe("verified_displayed");

    const second = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      // A different wall clock: only the timestamp module changes, which is
      // exactly what the sentinel clock exists to ignore.
      now: new Date(FIXTURE_NOW.getTime() + 3_600_000),
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    expect(second.outcome).toBe("would_dedup");
    expect(second.pushId).toBeNull();
    // No ledger lines were added: nothing was attempted.
    expect(readLedger()).toHaveLength(3);
  });

  it("proceeds when forced, and the device itself reports the dedup", async () => {
    await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });

    const forced = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      force: true,
      sleep: pacedSleep,
    });
    expect(forced.outcome).toBe("verified_displayed");
    expect(forced.deduped).toBe(true);
    expect(device.snapshot().refresh.skipped).toBe(1);
  });

  it("allows a push whose content actually changed", async () => {
    await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });

    const changed = doc();
    const message = changed.modules.find((m) => m.type === "message");
    if (message) {
      message.options = moduleDefinition("message").schema.parse({
        body: { text: "Nouvelle note" },
        expiresAt: null,
      }) as Record<string, unknown>;
    }

    const second = await push({
      doc: changed,
      version: 2,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    expect(second.outcome).toBe("verified_displayed");
    expect(second.deduped).toBe(false);
    expect(second.seq).toBe(2);
  });
});

describe("failure paths", () => {
  it("records a sha mismatch as failed, with nothing stored", async () => {
    // Corrupt the digest the client claims, so the device refuses the body.
    const original = DeviceClient.prototype.putFrame;
    DeviceClient.prototype.putFrame = function patched(bytes, options) {
      return original.call(this, bytes, { ...options, sha256: "f".repeat(64) });
    };
    try {
      const result = await push({
        doc: doc(),
        version: 1,
        sources: fixtureSources(),
        client,
        deviceMode: "mock",
        sleep: pacedSleep,
      });
      expect(result.outcome).toBe("failed");
      expect(result.detail).toMatch(/sha_mismatch/);
      expect(device.snapshot().stored.seq).toBe(0);
    } finally {
      DeviceClient.prototype.putFrame = original;
    }

    expect(readLedger().map((l) => l.state)).toEqual(["pending", "failed"]);
    // A clean failure does not block the next push.
    expect(blockingPush()).toBeNull();
  });

  it("records an unauthorised push as failed", async () => {
    const wrongToken = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: "d".repeat(64),
      timeoutMs: 2_000,
    });
    const result = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client: wrongToken,
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    expect(result.outcome).toBe("failed");
    expect(result.detail).toMatch(/unauthorized/);
  });

  it("records a lockout as failed and names the code", async () => {
    const wrongToken = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: "d".repeat(64),
      timeoutMs: 2_000,
    });
    for (let i = 0; i < 10; i += 1) {
      await push({
        doc: doc(),
        version: 1,
        sources: fixtureSources(),
        client: wrongToken,
        deviceMode: "mock",
        force: true,
        sleep: pacedSleep,
      });
    }
    const lockedOut = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client: wrongToken,
      deviceMode: "mock",
      force: true,
      sleep: pacedSleep,
    });
    expect(lockedOut.outcome).toBe("failed");
    expect(lockedOut.detail).toMatch(/locked_out/);
  });

  it("refuses to push with no token configured, before any request", async () => {
    const tokenless = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: null,
    });
    const before = device.requestLog.length;
    const result = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client: tokenless,
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    expect(result.outcome).toBe("failed");
    expect(device.requestLog.length).toBe(before);
  });
});

describe("uncertain paths", () => {
  it("becomes uncertain when the panel never acknowledges", async () => {
    await device.close();
    await makeClient({ failAck: true, panelDelayMs: 5 });

    const result = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      // A small budget stands in for the full 390 s worst case.
      maxWaitMs: 12_000,
    });

    expect(result.outcome).toBe("uncertain");
    expect(result.detail).toMatch(/never reported it displayed/);
    expect(readLedger().map((l) => l.state)).toEqual([
      "pending",
      "sent",
      "uncertain",
    ]);
  });

  it("blocks every later push until the uncertain one is resolved", async () => {
    await device.close();
    await makeClient({ failAck: true, panelDelayMs: 5 });

    await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      maxWaitMs: 12_000,
    });

    const blocked = await push({
      doc: doc("Autre"),
      version: 2,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
    });

    expect(blocked.outcome).toBe("blocked");
    expect(blocked.blockedBy?.state).toBe("uncertain");
    // No new intent was recorded and no second write went out.
    expect(readPushes()).toHaveLength(1);
    expect(
      device.requestLog.filter((entry) => entry.method === "PUT"),
    ).toHaveLength(1);
  });

  it("acknowledging an uncertain push unblocks the queue", async () => {
    await device.close();
    await makeClient({ failAck: true, panelDelayMs: 5 });

    const first = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      maxWaitMs: 12_000,
    });
    expect(blockingPush()).not.toBeNull();

    expect(acknowledge(first.pushId as string, "Checked the panel by hand")).toBe(
      true,
    );
    expect(blockingPush()).toBeNull();

    const audit = readAudit();
    expect(audit[0]).toMatchObject({
      action: "device.push.acknowledge",
      deviceConfirmed: false,
    });
  });

  it("re-checking confirms an uncertain push once the panel catches up", async () => {
    await device.close();
    await makeClient({ failAck: true, panelDelayMs: 5 });

    const first = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      maxWaitMs: 12_000,
    });
    expect(first.outcome).toBe("uncertain");

    // The panel recovers and repaints what was stored all along.
    device.failAck = false;
    await client.refresh();
    await new Promise((resolve) => setTimeout(resolve, 60));

    const rechecked = await recheck(first.pushId as string, client);
    expect(rechecked.state).toBe("verified_displayed");
    expect(blockingPush()).toBeNull();
    expect(lastVerifiedPush()?.pushId).toBe(first.pushId);
  });

  it("re-checking an unresolved push that is still wrong keeps it blocking", async () => {
    await device.close();
    await makeClient({ failAck: true, panelDelayMs: 5 });

    const first = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      maxWaitMs: 12_000,
    });

    const rechecked = await recheck(first.pushId as string, client);
    expect(rechecked.state).toBe("uncertain");
    expect(rechecked.detail).toMatch(/showing a different frame/);
    expect(blockingPush()).not.toBeNull();
  });

  it("reports an unknown push id rather than inventing one", async () => {
    expect(await recheck("not-a-push", client)).toEqual({
      state: "unknown",
      detail: "No such push in the ledger",
    });
    expect(acknowledge("not-a-push")).toBe(false);
  });
});

describe("write-ahead ordering", () => {
  it("has the pending line on disk before the PUT is answered", async () => {
    let ledgerAtPutTime: string[] = [];
    const original = DeviceClient.prototype.putFrame;
    DeviceClient.prototype.putFrame = async function patched(bytes, options) {
      // Read the ledger straight off disk from inside the network call.
      ledgerAtPutTime = fs
        .readFileSync(paths.ledger(), "utf8")
        .split("\n")
        .filter(Boolean)
        .map((line) => JSON.parse(line).state as string);
      return original.call(this, bytes, options);
    };
    try {
      await push({
        doc: doc(),
        version: 1,
        sources: fixtureSources(),
        client,
        deviceMode: "mock",
        sleep: pacedSleep,
      });
    } finally {
      DeviceClient.prototype.putFrame = original;
    }
    expect(ledgerAtPutTime).toEqual(["pending"]);
  });

  it("leaves a pending line behind when the process dies mid-PUT", async () => {
    const original = DeviceClient.prototype.putFrame;
    DeviceClient.prototype.putFrame = async function aborted() {
      throw new Error("process died");
    };
    try {
      await push({
        doc: doc(),
        version: 1,
        sources: fixtureSources(),
        client,
        deviceMode: "mock",
        sleep: pacedSleep,
      });
    } catch {
      // The pipeline turns this into a failed line; either way the intent is
      // durable, which is the point being tested.
    } finally {
      DeviceClient.prototype.putFrame = original;
    }

    const states = readLedger().map((line) => line.state);
    expect(states[0]).toBe("pending");
  });
});

describe("audit redaction", () => {
  it("replaces any secret-shaped parameter with a marker", () => {
    const redacted = redactParams({
      deviceAddress: "192.168.7.7",
      token: "abcdef",
      hubToken: "secret",
      passphrase: "hunter2",
      nested: { HASS_TOKEN: "value", label: "Capteur" },
      emptyToken: "",
    });
    expect(redacted).toEqual({
      deviceAddress: "192.168.7.7",
      token: "[redacted]",
      hubToken: "[redacted]",
      passphrase: "[redacted]",
      nested: { HASS_TOKEN: "[redacted]", label: "Capteur" },
      emptyToken: null,
    });
    expect(JSON.stringify(redacted)).not.toContain("hunter2");
    expect(JSON.stringify(redacted)).not.toContain("abcdef");
  });

  it("never writes the device token into the ledger", async () => {
    await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: pacedSleep,
    });
    const raw = fs.readFileSync(paths.ledger(), "utf8");
    expect(raw).not.toContain(TOKEN);
  });
});

/**
 * A push to a device that is not listening.
 *
 * The regression these exist for: a push to a panel in automatic power saving
 * was written down as `failed / unreachable` and thrown away. Nobody refused
 * anything — the radio was off, which is what that device does most of the
 * time — and the user's frame was simply gone.
 */
describe("queueing a push the device was not there to receive", () => {
  /** Queue one push against a sleeping mock and return what came back. */
  async function queueOne(title = "Kitchen panel") {
    device.asleep = true;
    const result = await push({
      doc: doc(title),
      version: 3,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      queueWhenUnreachable: true,
    });
    device.asleep = false;
    return result;
  }

  it("holds the frame instead of failing it, and sends nothing", async () => {
    const result = await queueOne();

    expect(result.outcome).toBe("queued");
    expect(result.detail).toMatch(/Held for the next time the device is reachable/);
    // The write-ahead ordering is unchanged: the intent is on disk first.
    expect(readLedger().map((line) => line.state)).toEqual(["pending", "queued"]);

    const queued = queuedPush();
    expect(queued?.pushId).toBe(result.pushId);
    expect(queued?.first.sha256).toBe(result.sha256);
    expect(queued?.latest.attempts).toBe(0);
    // Nothing was served: the mock records the attempt and destroys the
    // socket, exactly as a device with its radio off does.
    expect(
      device.requestLog.filter((e) => e.method === "PUT" && e.status !== 0),
    ).toHaveLength(0);
    expect(device.snapshot().stored.seq).toBe(0);
  });

  it("keeps the exact bytes, on disk, so a restart loses nothing", async () => {
    const result = await queueOne();

    // Read straight off the filesystem: this is what survives a process that
    // stops existing between the request and the device's next wake. The
    // bytes are checked against the digest the ledger recorded, because that
    // digest is this push's identity and the thing the device's own displayed
    // hash is later compared against.
    const onDisk = fs.readFileSync(paths.queuedFrame(result.pushId as string));
    expect(onDisk).toHaveLength(30_000);
    expect(createHash("sha256").update(onDisk).digest("hex")).toBe(result.sha256);
    expect(readQueuedFrame(result.pushId as string)).toEqual(new Uint8Array(onDisk));
    // And the ledger line on disk carries the same digest, so the frame and
    // its identity cannot drift apart.
    const lines = fs
      .readFileSync(paths.ledger(), "utf8")
      .split("\n")
      .filter(Boolean)
      .map((line) => JSON.parse(line) as { state: string; sha256: string });
    expect(lines.at(-1)).toMatchObject({ state: "queued", sha256: result.sha256 });
  });

  it("blocks a second push rather than stacking two frames on one wake", async () => {
    await queueOne();
    const second = await push({
      doc: doc("Autre"),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      force: true,
      sleep: instantSleep,
      queueWhenUnreachable: true,
    });
    expect(second.outcome).toBe("blocked");
    expect(second.blockedBy?.state).toBe("queued");
    expect(readPushes()).toHaveLength(1);
  });

  /**
   * The line between "nobody was home" and "the device said no". A queue that
   * swallowed the second would turn a five-second fix — a wrong token, a
   * digest the device rejected — into an hour of a spinner.
   */
  it("never queues a device that answered and refused", async () => {
    const wrongToken = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: "d".repeat(64),
      timeoutMs: 2_000,
    });
    const result = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client: wrongToken,
      deviceMode: "mock",
      sleep: instantSleep,
      queueWhenUnreachable: true,
    });
    expect(result.outcome).toBe("failed");
    expect(result.detail).toMatch(/unauthorized/);
    expect(queuedPush()).toBeNull();
    expect(fs.existsSync(paths.queuedFrame(result.pushId as string))).toBe(false);
  });

  it("never queues a tower that has no token configured", async () => {
    const tokenless = new DeviceClient({
      mode: "mock",
      address: "192.168.7.7",
      mockOrigin: device.origin,
      token: null,
    });
    const result = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client: tokenless,
      deviceMode: "mock",
      sleep: instantSleep,
      queueWhenUnreachable: true,
    });
    expect(result.outcome).toBe("failed");
    expect(queuedPush()).toBeNull();
  });

  /**
   * The default is off: queuing changes what an unreachable write means, so a
   * caller has to opt in rather than have every failed push silently held. Both
   * real callers now do (the push route and the automatic refresh); this pins
   * that the primitive itself still fails closed for one that does not.
   */
  it("fails, as before, for a caller that did not ask to queue", async () => {
    device.asleep = true;
    const result = await push({
      doc: doc(),
      version: 1,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
    });
    device.asleep = false;
    expect(result.outcome).toBe("failed");
    expect(queuedPush()).toBeNull();
    expect(readLedger().map((line) => line.state)).toEqual(["pending", "failed"]);
  });

  it("records the queue as pending in the audit log, not as a success", async () => {
    await queueOne();
    expect(readAudit()[0]).toMatchObject({
      action: "device.push",
      outcome: "pending",
      deviceConfirmed: false,
    });
  });

  it("can be withdrawn, which forgets the bytes and unblocks the queue", async () => {
    const result = await queueOne();
    expect(acknowledge(result.pushId as string)).toBe(true);
    expect(queuedPush()).toBeNull();
    expect(blockingPush()).toBeNull();
    expect(fs.existsSync(paths.queuedFrame(result.pushId as string))).toBe(false);
    expect(readAudit()[0]).toMatchObject({
      action: "device.push.cancel",
      // Nothing ever reached the device, so there is no device fact to record.
      deviceConfirmed: null,
    });
  });
});

describe("delivering a queued push", () => {
  async function queueOne() {
    device.asleep = true;
    const result = await push({
      doc: doc(),
      version: 3,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      queueWhenUnreachable: true,
    });
    device.asleep = false;
    return result;
  }

  /**
   * Frame writes the mock actually served.
   *
   * Status 0 is the mock destroying the socket, which is what it does while
   * asleep — an attempt that reached nothing. Counting those would make
   * "exactly once" a statement about the tower's optimism rather than about
   * the panel.
   */
  const puts = (): number =>
    device.requestLog.filter((entry) => entry.method === "PUT" && entry.status !== 0)
      .length;

  it("writes nothing at all while the device is still away", async () => {
    await queueOne();
    const before = readLedger().length;

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status: null,
      reachable: false,
    });

    expect(result.outcome).toBe("waiting");
    expect(result.attempted).toBe(false);
    expect(result.detail).toMatch(/held here/);
    // No ledger line, no attempt counted, no socket. The scheduler comes round
    // every thirty seconds; a line per pass would be a hundred and twenty an
    // hour saying nothing happened.
    expect(readLedger()).toHaveLength(before);
    expect(queuedPush()?.latest.attempts).toBe(0);
    expect(puts()).toBe(0);
  });

  it("sends it exactly once when the device answers, and verifies on the glass", async () => {
    const queuedResult = await queueOne();
    const status = await client.status();

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      sleep: pacedSleep,
    });

    expect(result.outcome).toBe("verified_displayed");
    expect(result.attempted).toBe(true);
    expect(puts()).toBe(1);

    // The same push, carried through: one identity, one digest, one record.
    const record = readPushes()[0];
    expect(record?.pushId).toBe(queuedResult.pushId);
    expect(record?.state).toBe("verified_displayed");
    expect(record?.history.map((line) => line.state)).toEqual([
      "pending",
      "queued",
      "sent",
      "verified_displayed",
    ]);
    expect(lastVerifiedPush()?.pushId).toBe(queuedResult.pushId);
    expect(resolveSha(queuedResult.sha256 as string)).toMatchObject({ version: 3 });

    // Nothing is blocking, and the tower is no longer holding the bytes.
    expect(blockingPush()).toBeNull();
    expect(fs.existsSync(paths.queuedFrame(queuedResult.pushId as string))).toBe(false);
    expect(readAudit()[0]).toMatchObject({
      action: "device.push.deliver",
      outcome: "ok",
      deviceConfirmed: true,
    });
  });

  it("does nothing when nothing is queued", async () => {
    expect(await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status: null,
      reachable: true,
    })).toEqual({
      outcome: "none",
      pushId: null,
      detail: "",
      attempted: false,
    });
  });

  /**
   * Idempotence, from the other end. An attempt can land and its answer be
   * lost — the device painted the frame and the socket died before the 202
   * came back. The next window must not repaint it.
   */
  it("verifies rather than re-sends a frame the panel is already showing", async () => {
    const queuedResult = await queueOne();
    const bytes = readQueuedFrame(queuedResult.pushId as string);
    if (!bytes) throw new Error("the queued frame should be on disk");

    // The attempt whose answer was lost.
    await client.putFrame(bytes, {
      sha256: queuedResult.sha256,
      idempotencyKey: "a-lost-answer",
    });
    await new Promise((resolve) => setTimeout(resolve, 60));
    const status = await client.status();
    expect(status.displayed.sha256).toBe(queuedResult.sha256);

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      sleep: pacedSleep,
    });

    expect(result.outcome).toBe("verified_displayed");
    expect(result.attempted).toBe(false);
    expect(result.detail).toMatch(/already displaying/);
    // One write in total, and it was the earlier one. The panel was not
    // refreshed a second time for a frame already on the glass.
    expect(puts()).toBe(1);
    expect(device.snapshot().refresh.renders).toBe(1);
  });

  it("keeps it queued, and counts one attempt, when the write does not land", async () => {
    await queueOne();
    const status = await client.status();
    // It went back to sleep between the status read and the write, which is
    // the whole shape of a bounded wake window.
    device.asleep = true;

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      sleep: instantSleep,
    });
    device.asleep = false;

    expect(result.outcome).toBe("retry_queued");
    expect(result.attempted).toBe(true);
    const queued = queuedPush();
    expect(queued?.state).toBe("queued");
    expect(queued?.latest.attempts).toBe(1);
    // Still held: the bytes are the thing that must survive.
    expect(readQueuedFrame(queued?.pushId as string)).not.toBeNull();
  });

  it("backs off instead of re-sending on the very next pass", async () => {
    await queueOne();
    const status = await client.status();
    device.asleep = true;
    await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      sleep: instantSleep,
    });
    device.asleep = false;

    const before = readLedger().length;
    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      sleep: instantSleep,
    });
    expect(result.outcome).toBe("backoff");
    expect(result.attempted).toBe(false);
    expect(readLedger()).toHaveLength(before);
    // The one attempt above reached a sleeping device and was destroyed, so
    // the panel has still never been written to.
    expect(puts()).toBe(0);
  });

  it("gives up after a bounded number of attempts, and says which", async () => {
    const queuedResult = await queueOne();
    const status = await client.status();
    device.asleep = true;

    let now = new Date();
    for (let attempt = 1; attempt <= MAX_QUEUED_PUSH_ATTEMPTS; attempt += 1) {
      const result = await deliverQueuedPush({
        client,
        deviceMode: "mock",
        status,
        reachable: true,
        now,
        sleep: instantSleep,
      });
      expect(result.outcome).toBe("retry_queued");
      // Past the backoff for the next one, and well inside the age limit.
      now = new Date(now.getTime() + 40 * 60_000);
    }
    device.asleep = false;

    const final = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      now,
      sleep: instantSleep,
    });
    expect(final.outcome).toBe("expired");
    expect(final.detail).toMatch(new RegExp(`${MAX_QUEUED_PUSH_ATTEMPTS} times`));

    const record = readPushes()[0];
    expect(record?.state).toBe("failed");
    expect(record?.latest.errorCode).toBe("queue_expired");
    expect(blockingPush()).toBeNull();
    expect(fs.existsSync(paths.queuedFrame(queuedResult.pushId as string))).toBe(false);
  });

  it("drops a frame that has grown too old to be worth painting", async () => {
    const queuedResult = await queueOne();
    const status = await client.status();

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      now: new Date(Date.now() + QUEUED_PUSH_MAX_AGE_MS + 60_000),
      sleep: instantSleep,
    });

    expect(result.outcome).toBe("expired");
    expect(result.detail).toMatch(/Nothing was sent/);
    expect(puts()).toBe(0);
    expect(readPushes()[0]?.state).toBe("failed");
    expect(fs.existsSync(paths.queuedFrame(queuedResult.pushId as string))).toBe(false);
    expect(readAudit()[0]).toMatchObject({
      action: "device.push.queue.expired",
      outcome: "failed",
      deviceConfirmed: false,
    });
  });

  /**
   * A device that answers and refuses is a bounded failure, immediately. It
   * does not become a retry, and it does not become a queue: the panel was
   * re-paired with something else, and no amount of waiting changes that.
   */
  it("fails at once, and forgets the bytes, when the device refuses permanently", async () => {
    const queuedResult = await queueOne();
    const status = await client.status();
    device.token = null;

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status,
      reachable: true,
      sleep: instantSleep,
    });

    expect(result.outcome).toBe("failed");
    expect(result.detail).toMatch(/not_provisioned/);
    expect(queuedPush()).toBeNull();
    expect(readPushes()[0]?.state).toBe("failed");
    expect(blockingPush()).toBeNull();
    expect(fs.existsSync(paths.queuedFrame(queuedResult.pushId as string))).toBe(false);
    expect(readAudit()[0]).toMatchObject({
      action: "device.push.deliver",
      outcome: "failed",
      deviceConfirmed: false,
    });
  });

  it("fails honestly if its own frame has gone missing, rather than re-rendering", async () => {
    const queuedResult = await queueOne();
    fs.unlinkSync(paths.queuedFrame(queuedResult.pushId as string));

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status: await client.status(),
      reachable: true,
      sleep: instantSleep,
    });

    expect(result.outcome).toBe("failed");
    expect(result.detail).toMatch(/missing from the tower's own store/);
    expect(puts()).toBe(0);
    expect(readPushes()[0]?.latest.errorCode).toBe("frame_missing");
  });

  /**
   * Switching back to the mock is not gated on an unresolved push — going back
   * to the simulator is the safe direction — so the guard has to be here. A
   * frame rendered for and recorded against the real panel must never be
   * delivered to the mock, or the ledger would carry a line saying a real push
   * was displayed when no hardware was involved.
   */
  it("refuses to deliver a frame to a device other than the one it was queued for", async () => {
    const queuedResult = await queueOne();
    const before = readLedger().length;

    const result = await deliverQueuedPush({
      client,
      deviceMode: "real",
      status: await client.status(),
      reachable: true,
      sleep: instantSleep,
    });

    expect(result.outcome).toBe("mode_changed");
    expect(result.attempted).toBe(false);
    expect(result.detail).toMatch(/Nothing was sent/);
    // Held, not failed: pointing the tower back at the panel delivers it.
    expect(readLedger()).toHaveLength(before);
    expect(queuedPush()?.pushId).toBe(queuedResult.pushId);
    expect(puts()).toBe(0);
  });

  it("is uncertain, never retried, when the panel does not confirm", async () => {
    const queuedResult = await queueOne();
    device.failAck = true;

    const result = await deliverQueuedPush({
      client,
      deviceMode: "mock",
      status: await client.status(),
      reachable: true,
      sleep: instantSleep,
      maxWaitMs: 12_000,
    });

    expect(result.outcome).toBe("uncertain");
    expect(readPushes()[0]?.state).toBe("uncertain");
    // Still blocking, because nobody can say what the panel is showing — but
    // no longer queued, because the bytes did go out and must never go again.
    expect(blockingPush()?.state).toBe("uncertain");
    expect(queuedPush()).toBeNull();
    expect(fs.existsSync(paths.queuedFrame(queuedResult.pushId as string))).toBe(false);
  });
});

/**
 * Delivering to a MARGINALLY reachable battery panel.
 *
 * The failure a real auto_saver cycle exposed: the connection is accepted mid-
 * wake (the small status read lands), then the 30 kB frame PUT hangs and times
 * out. For a person's push that is honestly `uncertain` and it blocks until they
 * resolve it. For the scheduler's own frame it must NOT wedge: the fixed
 * idempotency key makes a re-send a replay, never a second repaint, so the frame
 * is held for another attempt inside the same wake and delivered when the link
 * steadies — or bounded out by the attempt budget if it never does.
 */
describe("delivering an automatic frame to a marginal panel", () => {
  /** Render and hold an automatic frame for the mock. */
  async function enqueueAuto(): Promise<string> {
    const result = await enqueueFrame({
      doc: doc(),
      version: 3,
      sources: fixtureSources(),
      deviceMode: "mock",
      origin: "auto",
      now: FIXTURE_NOW,
    });
    expect(result.outcome).toBe("queued");
    return result.pushId as string;
  }

  /** Frame writes the mock actually served (status 0 is a hung/asleep attempt). */
  const putsServed = (): number =>
    device.requestLog.filter((e) => e.method === "PUT" && e.status !== 0).length;

  it("holds a hung PUT for another attempt instead of wedging on uncertain", async () => {
    const pushId = await enqueueAuto();
    device.frameStallMs = 300; // accepted, then the write never answers
    const status = await client.status(); // the status read still lands

    const result = await deliverQueuedPush({
      client,
      status,
      reachable: true,
      deviceMode: "mock",
      now: new Date(FIXTURE_NOW.getTime() + 1_000),
      sleep: instantSleep,
      uploadTimeoutMs: 50, // time the write out before the mock lets the socket go
    });

    // Held for retry: not a terminal state, and the bytes are kept.
    expect(result.outcome).toBe("retry_queued");
    const queued = queuedPush();
    expect(queued?.pushId).toBe(pushId);
    expect(queued?.state).toBe("queued");
    expect(queued?.latest.attempts).toBe(1);
    expect(queued?.latest.errorCode).toBe("uncertain");
    expect(readQueuedFrame(pushId)).not.toBeNull();
    // Nothing dishonest was recorded: no displayed, no failed, no terminal uncertain.
    const states = readLedger().map((line) => line.state);
    expect(states).not.toContain("uncertain");
    expect(states).not.toContain("failed");
    expect(states).not.toContain("verified_displayed");
    // The mock saw the write attempt but served nothing under it.
    expect(device.requestLog.some((e) => e.method === "PUT" && e.status === 0)).toBe(true);
    expect(putsServed()).toBe(0);
  });

  it("delivers to verified_displayed on a steadier attempt in the same wake", async () => {
    const pushId = await enqueueAuto();

    // First attempt hangs and is held.
    device.frameStallMs = 300;
    const first = await deliverQueuedPush({
      client,
      status: await client.status(),
      reachable: true,
      deviceMode: "mock",
      now: new Date(FIXTURE_NOW.getTime() + 1_000),
      sleep: instantSleep,
      uploadTimeoutMs: 50,
    });
    expect(first.outcome).toBe("retry_queued");

    // The link steadies later in the same wake; past the ten-second automatic
    // backoff the next attempt lands and the panel confirms it displayed.
    device.frameStallMs = 0;
    const second = await deliverQueuedPush({
      client,
      status: await client.status(),
      reachable: true,
      deviceMode: "mock",
      now: new Date(FIXTURE_NOW.getTime() + 12_000),
      sleep: pacedSleep,
      maxWaitMs: 5_000,
    });

    expect(second.outcome).toBe("verified_displayed");
    expect(queuedPush()).toBeNull();
    expect(findPush(pushId)?.state).toBe("verified_displayed");
    expect(readLedger().some((line) => line.state === "failed")).toBe(false);
    // Exactly one write ever actually landed on the panel.
    expect(putsServed()).toBe(1);
  });

  it("waits out the automatic backoff between attempts rather than re-sending every tick", async () => {
    await enqueueAuto();
    device.frameStallMs = 300;
    const first = await deliverQueuedPush({
      client,
      status: await client.status(),
      reachable: true,
      deviceMode: "mock",
      now: new Date(FIXTURE_NOW.getTime() + 1_000),
      sleep: instantSleep,
      uploadTimeoutMs: 50,
    });
    expect(first.outcome).toBe("retry_queued");

    // Three seconds later, well inside the ten-second window: the tower holds
    // and writes nothing, so `attempts` counts the wire and not the clock.
    const tooSoon = await deliverQueuedPush({
      client,
      status: await client.status(),
      reachable: true,
      deviceMode: "mock",
      now: new Date(FIXTURE_NOW.getTime() + 4_000),
      sleep: instantSleep,
      uploadTimeoutMs: 50,
    });
    expect(tooSoon.outcome).toBe("backoff");
    expect(tooSoon.attempted).toBe(false);
    expect(queuedPush()?.latest.attempts).toBe(1);
  });

  it("keeps the manual contract: a person's frame that times out stays uncertain and blocks", async () => {
    // A person's frame, queued while the panel was briefly unreachable.
    device.asleep = true;
    const manual = await push({
      doc: doc(),
      version: 3,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: instantSleep,
      queueWhenUnreachable: true,
    });
    expect(manual.outcome).toBe("queued");

    // Now awake but marginal: the write times out.
    device.asleep = false;
    device.frameStallMs = 300;
    const result = await deliverQueuedPush({
      client,
      status: await client.status(),
      reachable: true,
      deviceMode: "mock",
      now: new Date(FIXTURE_NOW.getTime() + 1_000),
      sleep: instantSleep,
      uploadTimeoutMs: 50,
    });

    // Strict: an unconfirmed manual write is uncertain, the bytes are dropped,
    // and it blocks until a person resolves it. No automatic retry.
    expect(result.outcome).toBe("uncertain");
    expect(queuedPush()).toBeNull();
    expect(readQueuedFrame(manual.pushId as string)).toBeNull();
    expect(blockingPush()?.pushId).toBe(manual.pushId);
    expect(findPush(manual.pushId as string)?.state).toBe("uncertain");
  });
});
