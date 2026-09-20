import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { MockDevice } from "@mock/server";
import { createDashboard, selectDashboard } from "@/server/store/dashboards";
import {
  OVERVIEW_SNAPSHOT_TTL_MS,
  invalidateOverviewSnapshot,
  readOverviewSnapshot,
  resetOverviewSnapshotForTests,
} from "@/server/overview/snapshot";
import { getMockDevice } from "@/server/device/mock";
import { useTempDataRoot } from "./helpers/tempRoot";
import type { DashboardDoc } from "@/core/model";

/**
 * The Overview snapshot, which did not exist.
 *
 * `GET /api/overview` used to read the device, collect every source and render
 * a full frame on every request — and the page polls it once a minute for as
 * long as the tab is open. Every test in this file fails against that code,
 * because there was nothing between the poll and the panel.
 *
 * The mock counts device reads, which is the measurement that matters: the
 * question is not whether the numbers are right but how many times the tower
 * opened a socket at a device that serves four of them and is asleep most of
 * the time.
 */

let temp: ReturnType<typeof useTempDataRoot>;
let device: MockDevice;
let doc: DashboardDoc;
let statusReads: number;
/** The mock is a process-wide singleton, so the counter has to be unwrapped. */
let restoreHandle: () => void;

const NOW = new Date("2026-09-14T10:00:00.000Z");

beforeEach(async () => {
  temp = useTempDataRoot();
  await resetOverviewSnapshotForTests();

  // The same in-process mock the server uses, started on demand. Nothing here
  // reaches a network or a real panel.
  device = (await getMockDevice()).device;
  device.reset();

  // Count the status requests the tower actually makes. `handle` is the
  // mock's HTTP entry point, so this counts sockets rather than field reads —
  // which is the measurement the whole snapshot exists to reduce.
  statusReads = 0;
  type Handled = {
    handle: (request: { url?: string }, response: unknown) => Promise<void>;
  };
  const target = device as unknown as Handled;
  const inner = target.handle.bind(device);
  target.handle = async (request, response) => {
    if ((request.url ?? "").startsWith("/api/v1/dashboard/status")) {
      statusReads += 1;
    }
    return inner(request, response);
  };
  restoreHandle = () => {
    target.handle = inner;
  };

  const record = createDashboard("Kitchen panel");
  doc = record.doc;
  selectDashboard(doc.id);
});

afterEach(async () => {
  // Drained before the handle is unwrapped, so a read this test started cannot
  // be counted against the next one.
  await resetOverviewSnapshotForTests();
  restoreHandle();
  vi.restoreAllMocks();
  temp.dispose();
});

/**
 * The first read, after the loading regression.
 *
 * A request that does not force must never wait on a device — and "must never"
 * now includes the case where there is no snapshot to serve, which is every
 * first page load after a restart. That case used to block on `build()`, and
 * `build()` blocks on a status read: two of them per Overview load once the
 * shell's own read is counted, serialised through one device mutex. Against a
 * panel that accepts a socket and never answers that was twenty seconds of
 * skeleton on a page that is nine tenths local files.
 */
describe("the first read does not wait on the device", () => {
  it("answers immediately with a snapshot that says it has read nothing", async () => {
    const before = statusReads;
    const read = await readOverviewSnapshot({ doc, now: NOW });

    // Not a failed read. Nothing was asked.
    expect(read.snapshot.device.observed).toBe(false);
    expect(read.snapshot.device.reachable).toBe(false);
    // No observation, so no observation time. Null rather than a stamp the
    // page would render as if a device had been contacted.
    expect(read.snapshot.observedAt).toBeNull();
    expect(read.stale).toBe(false);
    // ...and the real read was started behind the response.
    expect(read.refreshing).toBe(true);

    await vi.waitFor(() => {
      expect(statusReads).toBeGreaterThan(before);
    });
  });

  it("serves the real reading to the next poll, once it lands", async () => {
    await readOverviewSnapshot({ doc, now: NOW });
    await vi.waitFor(() => {
      expect(statusReads).toBeGreaterThan(0);
    });

    const settled = await readOverviewSnapshot({
      doc,
      now: new Date(NOW.getTime() + 1_000),
    });
    expect(settled.snapshot.device.observed).toBe(true);
    expect(settled.snapshot.device.reachable).toBe(true);
    expect(settled.snapshot.observedAt).toBe(NOW.toISOString());
  });

  it("never caches the unread snapshot, so a dead device is retried", async () => {
    // A tower whose panel is permanently away must keep trying, not settle
    // into answering from a placeholder forever.
    await readOverviewSnapshot({ doc, now: NOW });
    await resetOverviewSnapshotForTests();
    const before = statusReads;
    await readOverviewSnapshot({ doc, now: NOW });
    await vi.waitFor(() => {
      expect(statusReads).toBeGreaterThan(before);
    });
  });
});

/** Seed the cache the way the background refresh does, and wait for it. */
async function seedSnapshot(now: Date): Promise<string> {
  const read = await readOverviewSnapshot({ doc, now, force: true });
  return read.snapshot.observedAt as string;
}

describe("a poll does not reach the device", () => {
  it("reads the device once and serves the next sixty polls from the snapshot", async () => {
    await seedSnapshot(NOW);
    const first = await readOverviewSnapshot({ doc, now: NOW });
    expect(first.snapshot.device.reachable).toBe(true);
    const afterFirst = statusReads;
    expect(afterFirst).toBeGreaterThan(0);

    // Sixty minutes of polling, one poll a minute, all inside the TTL of the
    // first few and stale after that.
    for (let minute = 1; minute <= 4; minute += 1) {
      const read = await readOverviewSnapshot({
        doc,
        now: new Date(NOW.getTime() + minute * 60_000),
      });
      expect(read.stale).toBe(false);
      expect(read.snapshot.observedAt).toBe(first.snapshot.observedAt);
    }
    expect(statusReads).toBe(afterFirst);
  });

  it("answers a stale poll from the cache immediately and refreshes behind it", async () => {
    const firstObservedAt = await seedSnapshot(NOW);
    const afterFirst = statusReads;

    const later = new Date(NOW.getTime() + OVERVIEW_SNAPSHOT_TTL_MS + 1_000);
    const read = await readOverviewSnapshot({ doc, now: later });

    // The polling request got the OLD snapshot, and got it without waiting.
    expect(read.stale).toBe(true);
    expect(read.snapshot.observedAt).toBe(firstObservedAt);
    expect(read.refreshing).toBe(true);

    // The refresh happens behind it, and the next poll sees the new reading.
    await vi.waitFor(() => {
      expect(statusReads).toBeGreaterThan(afterFirst);
    });
    const next = await readOverviewSnapshot({ doc, now: later });
    expect(next.stale).toBe(false);
    expect(next.snapshot.observedAt).toBe(later.toISOString());
  });
});

describe("what still reaches the device", () => {
  it("forces, and waits, when a person asks to read again", async () => {
    await seedSnapshot(NOW);
    const afterFirst = statusReads;

    const forced = await readOverviewSnapshot({ doc, now: NOW, force: true });
    expect(statusReads).toBeGreaterThan(afterFirst);
    expect(forced.stale).toBe(false);
    // Waited for: the answer describes the read that just happened.
    expect(forced.snapshot.observedAt).toBe(NOW.toISOString());
  });

  it("re-reads when something changed the device under the cache", async () => {
    await seedSnapshot(NOW);
    const afterFirst = statusReads;

    // What the push route, the power route and the test hook all call.
    invalidateOverviewSnapshot();

    await readOverviewSnapshot({ doc, now: new Date(NOW.getTime() + 1_000) });
    await vi.waitFor(() => {
      expect(statusReads).toBeGreaterThan(afterFirst);
    });
  });

  it("never answers about one dashboard with another dashboard's sources", async () => {
    const firstObservedAt = await seedSnapshot(NOW);

    const other = createDashboard("Hallway panel").doc;
    const read = await readOverviewSnapshot({
      doc: other,
      now: new Date(NOW.getTime() + 1_000),
    });
    // Not stale — wrong. A snapshot of a different dashboard is not an old
    // answer to this question, it is an answer to a different one, so it is
    // not served at all: this comes back unread, with the right read started
    // behind it.
    expect(read.stale).toBe(false);
    expect(read.snapshot.dashboardId).toBe(other.id);
    expect(read.snapshot.observedAt).not.toBe(firstObservedAt);
    expect(read.snapshot.device.observed).toBe(false);
  });

  it("collapses concurrent refreshes into one device read", async () => {
    // Two browser tabs and a slow panel used to be two concurrent status reads
    // at a device that serves four sockets and runs its own UI off the same
    // server.
    await resetOverviewSnapshotForTests();
    const before = statusReads;
    const [a, b, c] = await Promise.all([
      readOverviewSnapshot({ doc, now: NOW, force: true }),
      readOverviewSnapshot({ doc, now: NOW, force: true }),
      readOverviewSnapshot({ doc, now: NOW, force: true }),
    ]);
    expect(a.snapshot.observedAt).toBe(b.snapshot.observedAt);
    expect(b.snapshot.observedAt).toBe(c.snapshot.observedAt);
    expect(statusReads - before).toBeLessThanOrEqual(1);
  });

  it("does not stack device reads when three unforced requests arrive at once", async () => {
    // The same property from the other side, and the one the page relies on:
    // a shell read, a page read and a poll landing together must still be one
    // socket at the panel — and none of the three may wait for it.
    await resetOverviewSnapshotForTests();
    const before = statusReads;
    const reads = await Promise.all([
      readOverviewSnapshot({ doc, now: NOW }),
      readOverviewSnapshot({ doc, now: NOW }),
      readOverviewSnapshot({ doc, now: NOW }),
    ]);
    for (const read of reads) expect(read.snapshot.device.observed).toBe(false);
    await vi.waitFor(() => {
      expect(statusReads).toBeGreaterThan(before);
    });
    expect(statusReads - before).toBeLessThanOrEqual(1);
  });
});

describe("the snapshot says how old it is", () => {
  it("carries the moment the device was read, not the moment it was served", async () => {
    const first = await readOverviewSnapshot({ doc, now: NOW, force: true });
    const later = new Date(NOW.getTime() + 2 * 60_000);
    const served = await readOverviewSnapshot({ doc, now: later });

    // The whole honesty argument for caching this at all: a page that showed
    // `later` here would be claiming a two-minute-fresh reading of a device it
    // had not contacted for two minutes.
    expect(served.snapshot.observedAt).toBe(first.snapshot.observedAt);
    expect(served.snapshot.observedAt).not.toBe(later.toISOString());
  });
});
