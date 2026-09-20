import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  createDashboard,
  readRecord,
  selectDashboard,
  updateDashboard,
} from "@/server/store/dashboards";
import { readState, updateState } from "@/server/store/state";
import {
  RefreshScheduler,
  runDeviceWindowTick,
  runRefreshTick,
} from "@/server/refreshScheduler";
import { deviceContext } from "@/server/device/context";
import { getMockDevice } from "@/server/device/mock";
import {
  appendLedger,
  findPush,
  queuedPush,
  readLedger,
  readPushes,
  type NewLedgerLine,
} from "@/server/device/ledger";
import { readIntent, recordIntent } from "@/server/device/powerIntent";
import { enqueueFrame, push, readQueuedFrame } from "@/server/device/pushPipeline";
import { fixtureSources } from "./fixtures/render";
import type { DashboardDoc } from "@/core/model";
import type { PushRequest } from "@/server/device/pushPipeline";
import { useTempDataRoot } from "./helpers/tempRoot";

let temp: ReturnType<typeof useTempDataRoot>;

beforeEach(() => {
  temp = useTempDataRoot();
});

afterEach(() => {
  temp.dispose();
  vi.restoreAllMocks();
});

function configured(minutes: 5 | 15 | 30 | 60 | 180 = 15) {
  const record = createDashboard("Scheduled");
  updateDashboard(record.doc.id, { ...record.doc, refreshIntervalMinutes: minutes });
  selectDashboard(record.doc.id);
  updateState({ deviceMode: "real" });
  return record.doc.id;
}

describe("refresh configuration", () => {
  it("defaults new dashboards to disabled", () => {
    expect(createDashboard("Manual").doc.refreshIntervalMinutes).toBeNull();
  });

  it("migrates old tower state with scheduler status disabled", () => {
    // Version 3 added the power intent. A tower upgrading into the hybrid
    // feature starts with no intent recorded and no last-seen time: a
    // back-filled timestamp would produce a confident and wrong next-wake
    // estimate the first time the device page was opened.
    expect(readState()).toMatchObject({
      schema_version: 3,
      lastAutoRefreshAt: null,
      lastAutoRefreshDashboardId: null,
      lastAutoRefreshOutcome: null,
      powerIntent: null,
      deviceLastSeenAt: null,
    });
  });
});

describe("runRefreshTick", () => {
  it("refreshes and pushes only the selected configured dashboard in real mode", async () => {
    const id = configured();
    const collect = vi.fn(async (_doc: DashboardDoc, _now: Date) => fixtureSources());
    const send = vi.fn(async (_request: Omit<PushRequest, "client">) => ({ outcome: "verified_displayed" as const }));
    const now = new Date("2026-09-12T10:15:00.000Z");

    expect(await runRefreshTick({ now, collect, send })).toBe("verified_displayed");
    expect(collect).toHaveBeenCalledOnce();
    const collectedDoc = collect.mock.calls[0]?.[0] as { id: string } | undefined;
    expect(collectedDoc?.id).toBe(id);
    expect(send).toHaveBeenCalledOnce();
    const sentRequest = send.mock.calls[0]?.[0] as unknown;
    expect(sentRequest).toMatchObject({
      doc: { id },
      deviceMode: "real",
      force: false,
      now,
    });
    expect(readState()).toMatchObject({
      lastAutoRefreshAt: now.toISOString(),
      lastAutoRefreshDashboardId: id,
      lastAutoRefreshOutcome: "verified_displayed",
    });
  });

  it("does nothing when disabled, unselected, mock, or not due", async () => {
    const collect = vi.fn(async (_doc: DashboardDoc, _now: Date) => fixtureSources());
    const send = vi.fn(async (_request: Omit<PushRequest, "client">) => ({ outcome: "verified_displayed" as const }));
    expect(await runRefreshTick({ collect, send })).toBe("disabled");

    const id = configured(15);
    updateState({ deviceMode: "mock" });
    expect(await runRefreshTick({ collect, send })).toBe("not_real");
    updateState({ deviceMode: "real", lastAutoRefreshAt: "2026-09-12T10:10:00.000Z", lastAutoRefreshDashboardId: id });
    expect(await runRefreshTick({ now: new Date("2026-09-12T10:20:00.000Z"), collect, send })).toBe("not_due");
    expect(collect).not.toHaveBeenCalled();
    expect(send).not.toHaveBeenCalled();
  });

  it("retries a failed or abandoned cycle after five minutes", async () => {
    const id = configured(60);
    const collect = vi.fn(async (_doc: DashboardDoc, _now: Date) => fixtureSources());
    const send = vi.fn(async (_request: Omit<PushRequest, "client">) => ({
      outcome: "verified_displayed" as const,
    }));

    for (const outcome of ["failed", "blocked", "refreshing"] as const) {
      updateState({
        lastAutoRefreshAt: "2026-09-12T10:10:00.000Z",
        lastAutoRefreshDashboardId: id,
        lastAutoRefreshOutcome: outcome,
      });
      expect(
        await runRefreshTick({
          now: new Date("2026-09-12T10:16:00.000Z"),
          collect,
          send,
        }),
      ).toBe("verified_displayed");
    }

    expect(send).toHaveBeenCalledTimes(3);
  });

  it("records source failures without opening the device pipeline", async () => {
    configured();
    const collect = vi.fn(async (_doc: DashboardDoc, _now: Date) => { throw new Error("offline"); });
    const send = vi.fn();
    expect(await runRefreshTick({ now: new Date("2026-09-12T10:15:00.000Z"), collect, send })).toBe("failed");
    expect(send).not.toHaveBeenCalled();
    expect(readState().lastAutoRefreshOutcome).toBe("failed");
  });
});

describe("RefreshScheduler", () => {
  it("prevents overlapping ticks", async () => {
    let release!: () => void;
    const tick = vi.fn(() => new Promise<void>((resolve) => { release = resolve; }));
    const scheduler = new RefreshScheduler(tick);
    const first = scheduler.tick();
    const second = scheduler.tick();
    expect(await second).toBe("overlap");
    expect(tick).toHaveBeenCalledOnce();
    release();
    expect(await first).toBe("ran");
  });
});

/**
 * The one background pass that touches a device which is usually away.
 *
 * Two things can be waiting on the same wake window — a power mode the device
 * has not heard, and a frame it was not there to receive — and the thing being
 * pinned here is that they cost ONE status read between them. The panel serves
 * four sockets and runs its own interface off the same server; a window where
 * it is finally awake is not the moment to knock twice.
 *
 * Everything here runs in mock mode, against the in-process mock. A unit test
 * must never drive the real-device branch: `resolveEndpoint` pins real mode to
 * port 80 on an RFC1918 address, so a test that took that path would open a
 * connection to whatever is at that address on the machine running the suite.
 */
describe("runDeviceWindowTick", () => {
  async function mock() {
    const { device } = await getMockDevice();
    return device;
  }

  async function queueAPush(): Promise<string> {
    const device = await mock();
    device.asleep = true;
    const { client } = await deviceContext();
    const record = createDashboard("Queued");
    const result = await push({
      doc: record.doc,
      version: record.latestVersion,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: async () => undefined,
      queueWhenUnreachable: true,
    });
    device.asleep = false;
    expect(result.outcome).toBe("queued");
    return result.pushId as string;
  }

  /** Status reads the mock served before the first frame write. */
  function statusReadsBeforeFirstPut(
    log: Array<{ method: string; path: string; status: number }>,
  ): number {
    const put = log.findIndex(
      (entry) => entry.method === "PUT" && entry.path.endsWith("/frame"),
    );
    const upTo = put === -1 ? log : log.slice(0, put);
    return upTo.filter(
      (entry) => entry.method === "GET" && entry.path.endsWith("/status"),
    ).length;
  }

  beforeEach(async () => {
    const device = await mock();
    device.reset();
    device.asleep = false;
    device.panelDelayMs = 5;
  });

  it("opens no socket at all when nothing is waiting", async () => {
    const device = await mock();
    expect(await runDeviceWindowTick()).toEqual({
      intent: "no_intent",
      queued: "none",
      reachable: null,
    });
    expect(device.requestLog).toHaveLength(0);
  });

  it("delivers a queued frame on one status read, and writes once", async () => {
    const pushId = await queueAPush();
    const device = await mock();
    device.requestLog.length = 0;

    const result = await runDeviceWindowTick();

    expect(result.queued).toBe("verified_displayed");
    expect(result.reachable).toBe(true);
    // The read that decided the device was there is the read the delivery
    // used. A second one before the write would be the door knocked twice.
    expect(statusReadsBeforeFirstPut(device.requestLog)).toBe(1);
    expect(
      device.requestLog.filter((e) => e.method === "PUT" && e.status !== 0),
    ).toHaveLength(1);

    expect(queuedPush()).toBeNull();
    expect(findPush(pushId)?.state).toBe("verified_displayed");
    // And the device was noted as seen, which is what the next-wake estimate
    // on every page is built from.
    expect(readState().deviceLastSeenAt).not.toBeNull();
  });

  it("waits, writing nothing, while the device is not answering", async () => {
    await queueAPush();
    const device = await mock();
    device.requestLog.length = 0;
    device.asleep = true;
    const before = readLedger().length;

    const result = await runDeviceWindowTick();
    device.asleep = false;

    expect(result).toMatchObject({ queued: "waiting", reachable: false });
    expect(readLedger()).toHaveLength(before);
    expect(queuedPush()?.latest.attempts).toBe(0);
    // One attempt to reach it, not a poll: a sleeping device is not a thing to
    // keep knocking at.
    expect(device.requestLog).toHaveLength(1);
    // Last-seen is not moved by a failed read, or the wake estimate would
    // march forward every time the device was asleep, which is most of the time.
    expect(readState().deviceLastSeenAt).toBeNull();
  });

  /**
   * The pass used to discard the reason outright — `catch {}` — and that is
   * how a transport failure spent days being reported as a sleeping panel.
   * The reason is now classified and handed back. What has NOT changed is that
   * a device which is away is still not an event: `notable` is false and this
   * pass still writes nothing, which the assertions above already pin.
   */
  it("hands back why the device did not answer instead of discarding it", async () => {
    await queueAPush();
    const device = await mock();
    device.asleep = true;

    const result = await runDeviceWindowTick();
    device.asleep = false;

    expect(result.reachable).toBe(false);
    expect(result.failure?.kind).toBe("absent");
    expect(result.failure?.notable).toBe(false);
  });

  it("carries both halves through one pass without either breaking the other", async () => {
    recordIntent({
      mode: "always_on",
      interactiveMinutes: null,
      wakeIntervalMinutes: null,
    });
    await queueAPush();

    const result = await runDeviceWindowTick();

    // The power contract stays a real-device one and the scheduler leaves the
    // mock's configuration alone, exactly as it did before the queue existed.
    expect(result.intent).toBe("not_real");
    expect(readIntent()).not.toBeNull();
    // The frame is delivered regardless: it is about whichever device the
    // tower is pointed at, and in mock mode that is the mock.
    expect(result.queued).toBe("verified_displayed");
  });
});

describe("the refresh pass and the queue", () => {
  it("refuses to render a second frame while one is already waiting", async () => {
    const id = configured(15);
    // The push itself is made in mock mode: a unit test must never build a
    // real-device client, which would resolve to port 80 on whatever is at
    // the configured address on the machine running the suite.
    updateState({ deviceMode: "mock" });
    const { device } = await getMockDevice();
    device.reset();
    device.asleep = true;
    const { client } = await deviceContext();
    const record = readRecord(id);
    if (!record) throw new Error("the dashboard should exist");
    const queued = await push({
      doc: record.doc,
      version: record.latestVersion,
      sources: fixtureSources(),
      client,
      deviceMode: "mock",
      sleep: async () => undefined,
      queueWhenUnreachable: true,
    });
    device.asleep = false;
    expect(queued.outcome).toBe("queued");

    updateState({ deviceMode: "real" });
    const collect = vi.fn(async () => fixtureSources());
    const send = vi.fn(async () => ({ outcome: "verified_displayed" as const }));

    expect(await runRefreshTick({ collect, send })).toBe("blocked");
    // And not one source was read to produce a frame that would be refused:
    // that is a forecast fetch, a Home Assistant read and a `remindctl`
    // process saved every five minutes for as long as the device is away.
    expect(collect).not.toHaveBeenCalled();
    expect(send).not.toHaveBeenCalled();
    expect(readState().lastAutoRefreshOutcome).toBe("blocked");
  });
});

/**
 * An unattended dashboard on a battery panel that is asleep — or only marginally
 * awake — almost all the time.
 *
 * The refresh pass no longer pushes. When a dashboard is due it RENDERS AND
 * QUEUES the frame, and the window pass delivers it: "a refresh is due" is
 * decoupled from "deliver when reachable". That is what closes two failures. A
 * fully-asleep panel used to get a `failed / unreachable` red badge for a frame
 * nobody refused; and a *marginally* awake one — the connection accepted, then
 * the PUT hanging into `uncertain` mid-wake — used to wedge the queue and waste
 * every other delivery chance in the same ninety-second wake.
 *
 * Because the refresh pass opens no socket, its real path can be exercised
 * directly here (no injected `send`, no real-device client): `enqueueFrame` only
 * renders and writes to the local ledger.
 */
describe("the refresh pass renders and queues, and never pushes", () => {
  async function mock() {
    return (await getMockDevice()).device;
  }

  /** Seed the ledger with an outstanding push of a given origin and state. */
  function seedPush(origin: "manual" | "auto", state: NewLedgerLine["state"]): string {
    const pushId = `seed_${Math.random().toString(36).slice(2)}`;
    const base = {
      pushId,
      dashboardId: "d_seed",
      dashboardTitle: "Seed",
      version: 1,
      sha256: "a".repeat(64),
      semanticHash: "b".repeat(64),
      idempotencyKey: `k_${pushId}`,
      deviceMode: "real" as const,
      origin,
      forced: false,
      seq: null,
      deduped: null,
      replay: null,
      render: null,
      panelMs: null,
      errorCode: null,
      attempts: 0,
      detail: null,
    } satisfies Omit<NewLedgerLine, "state">;
    appendLedger({ ...base, state: "pending" });
    appendLedger({ ...base, state });
    return pushId;
  }

  beforeEach(async () => {
    const device = await mock();
    device.reset();
    device.asleep = false;
    device.frameStallMs = 0;
    device.panelDelayMs = 5;
  });

  it("renders and queues a due refresh without opening a socket to the device", async () => {
    configured(30);
    const device = await mock();
    device.requestLog.length = 0;
    const collect = vi.fn(async () => fixtureSources());

    // No injected send: this is the real production path.
    const outcome = await runRefreshTick({
      now: new Date("2026-09-18T10:00:00.000Z"),
      collect,
    });

    expect(outcome).toBe("queued");
    // The refresh pass touches no wire at all — delivery is the window pass's job.
    expect(device.requestLog).toHaveLength(0);
    const queued = queuedPush();
    expect(queued?.first.origin).toBe("auto");
    expect(queued?.latest.attempts).toBe(0);
    // Rendered and held: one `queued` line, nothing `pending`, `sent` or `failed`.
    const states = readLedger()
      .filter((line) => line.pushId === queued?.pushId)
      .map((line) => line.state);
    expect(states).toEqual(["queued"]);
    expect(readLedger().some((line) => line.state === "failed")).toBe(false);
    // Recorded as queued, deliberately not a "recovering" state: the next render
    // waits a full interval, because delivery is the window pass's job.
    expect(readState().lastAutoRefreshOutcome).toBe("queued");
  });

  it("supersedes its own still-queued frame, collapsing two due cycles into one (latest wins)", async () => {
    configured(30);
    const collect = vi.fn(async () => fixtureSources());

    await runRefreshTick({ now: new Date("2026-09-18T10:00:00.000Z"), collect });
    const first = queuedPush();
    expect(first).not.toBeNull();

    // A full interval later, still never delivered: the held frame is replaced,
    // not stacked behind. Nothing reached the device, so nothing is painted over.
    const outcome = await runRefreshTick({
      now: new Date("2026-09-18T10:40:00.000Z"),
      collect,
    });

    expect(outcome).toBe("queued");
    const second = queuedPush();
    expect(second?.pushId).not.toBe(first?.pushId); // the newer render wins
    expect(findPush(first!.pushId)?.state).toBe("acknowledged");
    expect(readQueuedFrame(first!.pushId)).toBeNull();
    expect(readQueuedFrame(second!.pushId)).not.toBeNull();
    // Exactly one frame is ever held: the queue is bounded to the most recent.
    expect(readPushes().filter((record) => record.state === "queued")).toHaveLength(1);
    expect(readLedger().some((line) => line.state === "failed")).toBe(false);
  });

  it("is blocked by a still-outstanding MANUAL push, and reads no sources", async () => {
    configured(30);
    const uncertain = seedPush("manual", "uncertain");
    const collect = vi.fn(async () => fixtureSources());

    const outcome = await runRefreshTick({
      now: new Date("2026-09-18T10:00:00.000Z"),
      collect,
    });

    // A person's unconfirmed push keeps its strict contract: it blocks the
    // automatic pass until a human resolves it, and no frame is rendered.
    expect(outcome).toBe("blocked");
    expect(collect).not.toHaveBeenCalled();
    expect(findPush(uncertain)?.state).toBe("uncertain");
    expect(readState().lastAutoRefreshOutcome).toBe("blocked");
  });

  it("supersedes an AUTO frame left uncertain by a marginal delivery, rather than wedging", async () => {
    configured(30);
    // A prior automatic delivery reached the wire and never confirmed. Under the
    // old rules this `uncertain` blocked every later due frame forever.
    const stale = seedPush("auto", "uncertain");
    const collect = vi.fn(async () => fixtureSources());

    const outcome = await runRefreshTick({
      now: new Date("2026-09-18T10:00:00.000Z"),
      collect,
    });

    expect(outcome).toBe("queued");
    // The stale automatic frame is retired, and a fresh one takes its place.
    expect(findPush(stale)?.state).toBe("acknowledged");
    const queued = queuedPush();
    expect(queued?.first.origin).toBe("auto");
    expect(queued?.pushId).not.toBe(stale);
    expect(readPushes().filter((record) => record.state === "queued")).toHaveLength(1);
  });

  it("hands the queued auto frame to the window pass, which delivers it queued -> sent -> verified", async () => {
    // Delivery runs in mock mode, against the real mock, exactly as the
    // scheduler's window pass does in the field.
    updateState({ deviceMode: "mock" });
    const device = await mock();
    const record = createDashboard("Overnight");
    // Enqueue at wall-clock now: the window pass below reads the real clock, and
    // a frame queued days ago (a fixture time) would age past its six-hour life.
    const enqueued = await enqueueFrame({
      doc: record.doc,
      version: record.latestVersion,
      sources: fixtureSources(),
      deviceMode: "mock",
      origin: "auto",
      now: new Date(),
    });
    expect(enqueued.outcome).toBe("queued");
    const pushId = enqueued.pushId as string;

    // The panel is awake for its rendezvous window; the device pass delivers it.
    device.asleep = false;
    const result = await runDeviceWindowTick();

    expect(result.queued).toBe("verified_displayed");
    expect(findPush(pushId)?.state).toBe("verified_displayed");
    expect(queuedPush()).toBeNull();
    // Held, sent, then confirmed by the device's OWN displayed digest — never a
    // "failed" for a frame nobody refused.
    const states = readLedger()
      .filter((line) => line.pushId === pushId)
      .map((line) => line.state);
    expect(states).toEqual(["queued", "sent", "verified_displayed"]);
  });
});
