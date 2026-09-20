import type { DashboardDoc } from "@/core/model";
import type { DashboardSources } from "@/core/render/data";
import { deviceContext } from "./device/context";
import { describeDeviceFailure, type DeviceFailure } from "./device/failure";
import { blockingPush, queuedPush } from "./device/ledger";
import {
  noteDeviceSeen,
  readIntent,
  reconcileIntent,
} from "./device/powerIntent";
import {
  acknowledge,
  deliverQueuedPush,
  enqueueFrame,
  type PushRequest,
  type PushResult,
  type QueuedDeliveryOutcome,
} from "./device/pushPipeline";
import { collectSources } from "./sources";
import { readRecord } from "./store/dashboards";
import { readState, updateState } from "./store/state";

export type RefreshTickResult =
  | "disabled"
  | "not_real"
  | "not_due"
  | "failed"
  | PushResult["outcome"];

interface TickDependencies {
  now?: Date;
  collect?: (doc: DashboardDoc, now: Date) => Promise<DashboardSources>;
  send?: (request: Omit<PushRequest, "client">) => Promise<Pick<PushResult, "outcome">>;
}

/**
 * Retire one of the scheduler's own outstanding frames so a fresher render can
 * take its place.
 *
 * Used for a frame still held for a sleeping panel, and for one an automatic
 * delivery left unconfirmed (`sent`/`uncertain`) after reaching the wire. A
 * withdrawal, not a delivery: it appends a terminal `acknowledged` line and
 * drops any held bytes, exactly as an operator withdrawing the frame by hand
 * would, and records no device fact — the device's own displayed digest, not
 * the tower, remains the only thing that ever marks a frame delivered. The
 * append-only ledger keeps the whole story beside the replacement's lines, and
 * the replacement carries a fresh idempotency key, so nothing is painted twice.
 */
function supersedeAuto(pushId: string): void {
  acknowledge(
    pushId,
    "Superseded by a newer scheduled refresh before the previous one was confirmed",
  );
}

/** One durable scheduler pass. It never pushes an unselected dashboard or a mock. */
export async function runRefreshTick(deps: TickDependencies = {}): Promise<RefreshTickResult> {
  const state = readState();
  if (!state.selectedDashboardId) return "disabled";
  const record = readRecord(state.selectedDashboardId);
  const interval = record?.doc.refreshIntervalMinutes ?? null;
  if (!record || record.doc.status !== "active" || interval === null) return "disabled";
  if (state.deviceMode !== "real") return "not_real";

  const now = deps.now ?? new Date();
  if (state.lastAutoRefreshDashboardId === record.doc.id && state.lastAutoRefreshAt) {
    const elapsed = now.getTime() - new Date(state.lastAutoRefreshAt).getTime();
    const recovering =
      state.lastAutoRefreshOutcome === "failed" ||
      state.lastAutoRefreshOutcome === "blocked" ||
      state.lastAutoRefreshOutcome === "refreshing";
    const dueAfterMs = (recovering ? Math.min(interval, 5) : interval) * 60_000;
    if (Number.isFinite(elapsed) && elapsed >= 0 && elapsed < dueAfterMs) {
      return "not_due";
    }
  }

  // The scheduler owns the frames it produces, and supersedes its own rather
  // than stack on them. A push it queued earlier — still held for a panel that
  // never woke, or left `sent`/`uncertain` by a delivery that reached the wire
  // and never confirmed — is replaced by this fresh render: a new moment wants a
  // new frame, "latest wins" keeps at most one outstanding, and the replacement
  // carries a fresh idempotency key so nothing is painted twice. This is what
  // stops a single marginal wake wedging the queue forever.
  //
  // A MANUAL push is never touched here. An unconfirmed human push keeps its
  // strict "resolve it yourself" contract and blocks this pass — checked before
  // the sources are read, because collecting first would mean a forecast fetch,
  // a Home Assistant read and a `remindctl` process every recovery interval to
  // produce a frame that would only be refused. The timestamp is still written,
  // so the recovery interval applies and this does not re-run every thirty
  // seconds.
  const blocking = blockingPush();
  const superseding = blocking && blocking.first.origin === "auto" ? blocking : null;
  if (blocking && !superseding) {
    updateState({
      lastAutoRefreshAt: now.toISOString(),
      lastAutoRefreshDashboardId: record.doc.id,
      lastAutoRefreshOutcome: "blocked",
    });
    return "blocked";
  }

  const collect = deps.collect ?? collectSources;
  try {
    const sources = await collect(record.doc, now);
    // Persist freshness before opening a socket. A restart cannot duplicate this tick.
    updateState({
      lastAutoRefreshAt: now.toISOString(),
      lastAutoRefreshDashboardId: record.doc.id,
      lastAutoRefreshOutcome: "refreshing",
    });
    const baseRequest = {
      doc: record.doc,
      version: record.latestVersion,
      sources,
      deviceMode: "real" as const,
      force: false,
      now,
    };
    // Retire the previous frame the scheduler owned, then render and QUEUE the
    // new one. Never a blocking direct push: on a battery panel the wake window
    // is brief and marginal, so a synchronous push that lands during Wi-Fi
    // association or a moment before sleep hangs into `uncertain` and wedges the
    // whole wake. Producing the frame here and letting `runDeviceWindowTick`
    // deliver it decouples "a refresh is due" from "deliver when reachable", and
    // that pass — which runs every tick — retries across the several ticks in
    // one wake and confirms from the device's own displayed digest.
    if (superseding) supersedeAuto(superseding.pushId);
    let result: Pick<PushResult, "outcome">;
    if (deps.send) {
      result = await deps.send(baseRequest);
    } else {
      // No device socket is opened here: `enqueueFrame` renders and writes a
      // `queued` line with the bytes beside it, nothing more. So this runs in
      // real mode without building a real-device client at all.
      result = await enqueueFrame({ ...baseRequest, origin: "auto" });
    }
    updateState({ lastAutoRefreshOutcome: result.outcome });
    return result.outcome;
  } catch {
    updateState({
      lastAutoRefreshAt: now.toISOString(),
      lastAutoRefreshDashboardId: record.doc.id,
      lastAutoRefreshOutcome: "failed",
    });
    return "failed";
  }
}

export type PowerTickResult =
  | "no_intent"
  | "not_real"
  | "unreachable"
  | "applied"
  | "waiting"
  | "cleared";

/**
 * What the queued-frame half of a window pass did.
 *
 * `not_read` is the one value that is not a delivery outcome: the pass threw
 * before it could reach a verdict. The frame is untouched and still queued, so
 * the next window tries again — inside the same bounded attempt budget.
 */
export type QueuedTickResult = QueuedDeliveryOutcome | "not_read";

export interface DeviceWindowResult {
  /** What happened to a pending power intent. */
  intent: PowerTickResult;
  /** What happened to a queued frame. */
  queued: QueuedTickResult;
  /** Whether the device answered, or null when no socket was opened at all. */
  reachable: boolean | null;
  /**
   * Why the device did not answer, when it did not.
   *
   * Null when it answered and null when no socket was opened. This pass writes
   * nothing about a device that is away — see the catch below for why — but a
   * pass that gives up because the tower's own transport failed should at
   * least be able to say so to whoever asked it to run.
   */
  failure?: DeviceFailure | null;
}

/**
 * One durable pass at everything the tower owes a device that is usually away.
 *
 * This is the background half of the answer to "how does a request reach a
 * device that is asleep fifty-nine minutes out of sixty". The other half is
 * the routes, which try immediately while the user is there. Both go through
 * `reconcileIntent` and `deliverQueuedPush`, so they cannot disagree.
 *
 * Two things can be waiting — a power mode the device has not heard, and a
 * frame it was not there to receive — and they share ONE status read per pass.
 * That is the whole reason this is a single function rather than two ticks:
 * the panel serves four sockets and runs its own interface off the same
 * server, so a window where the device is finally awake is not the moment to
 * knock on the door twice.
 *
 * Order matters. The power intent goes first, because the most common intent
 * is "stay awake for fifteen minutes" and applying it widens the very window
 * the frame then needs. The frame goes second, because it is the slow one: the
 * poll that confirms it can run for minutes, and a device asked to stay awake
 * before that starts is a device that is still there when it finishes.
 *
 * Cheap when there is nothing to do: with no intent and nothing queued it
 * returns before opening a socket, which is the overwhelmingly common case.
 * Neither half may take the other down — a device that refuses a config write
 * must not stop a frame being delivered — so each is caught on its own.
 */
export async function runDeviceWindowTick(options: {
  now?: Date;
} = {}): Promise<DeviceWindowResult> {
  const now = options.now ?? new Date();
  const hasIntent = readIntent() !== null;
  const hasQueued = queuedPush() !== null;

  const state = readState();
  // The power contract is a real-device one and the scheduler has never
  // applied it to the mock. A queued frame is different: it is about whichever
  // device the tower is pointed at, the mock included, and `deliverQueuedPush`
  // refuses outright to send a frame to a device other than the one it was
  // queued for, so the two modes cannot bleed into each other.
  const intentWanted = hasIntent && state.deviceMode === "real";

  if (!intentWanted && !hasQueued) {
    return {
      intent: hasIntent ? "not_real" : "no_intent",
      queued: "none",
      reachable: null,
    };
  }

  const context = await deviceContext();
  if (context.simulated && !hasQueued) {
    return { intent: "not_real", queued: "none", reachable: null };
  }

  let status = null;
  try {
    status = await context.client.status();
  } catch (error) {
    // Asleep, almost certainly — and "almost" is the word that mattered. Still
    // not recorded: see the "waiting" branches of `reconcileIntent` and
    // `deliverQueuedPush`, which both write nothing, and which this pass calls
    // every thirty seconds whether or not anybody is home. A line per pass
    // would be a hundred and twenty an hour saying nothing happened.
    //
    // What is new is that the verdict is classified rather than discarded, and
    // handed back. A caller that wants to know whether this was a sleeping
    // panel or a transport that never left the machine can now ask, without
    // this function deciding to write to disk on its behalf.
    return {
      intent: hasIntent ? "unreachable" : "no_intent",
      queued: hasQueued ? "waiting" : "none",
      reachable: false,
      failure: describeDeviceFailure(error),
    };
  }

  noteDeviceSeen(now, status);

  // Each half is caught on its own. A device that refuses a configuration
  // write must not be the reason a frame somebody asked for is still sitting
  // on this machine, and a frame that fails to verify must not leave a mode
  // change undelivered.
  let intent: PowerTickResult = hasIntent ? "not_real" : "no_intent";
  if (intentWanted && !context.simulated) {
    try {
      const result = await reconcileIntent(context.client, status, {
        reachable: true,
        now,
      });
      intent = result.applied
        ? "applied"
        : result.intent === null
          ? "cleared"
          : "waiting";
    } catch {
      intent = "waiting";
    }
  }

  let queued: QueuedTickResult = "none";
  if (hasQueued) {
    try {
      const delivery = await deliverQueuedPush({
        client: context.client,
        status,
        reachable: true,
        deviceMode: context.simulated ? "mock" : "real",
        now,
      });
      queued = delivery.outcome;
    } catch {
      // The frame is still on disk and still queued: nothing here removes it,
      // so the next window tries again inside the same bounded budget.
      queued = "not_read";
    }
  }

  return { intent, queued, reachable: true, failure: null };
}

/**
 * The power half of a window pass, on its own.
 *
 * Kept as its own name because that is what a power intent's delivery is
 * called everywhere else, and because it is the contract the route-shape tests
 * pin. It is a view of `runDeviceWindowTick`, not a second implementation, so
 * there is still only one status read and one decision.
 */
export async function runPowerIntentTick(): Promise<PowerTickResult> {
  return (await runDeviceWindowTick()).intent;
}

export class RefreshScheduler {
  private running = false;
  constructor(private readonly work: () => Promise<unknown> = runRefreshTick) {}

  async tick(): Promise<"ran" | "overlap"> {
    if (this.running) return "overlap";
    this.running = true;
    try {
      await this.work();
      return "ran";
    } finally {
      this.running = false;
    }
  }
}

const GLOBAL_KEY = Symbol.for("note4c.refreshScheduler");
const GLOBAL_KEY_FAST = Symbol.for("note4c.refreshScheduler.fastCatch");
type SchedulerGlobal = typeof globalThis & {
  [GLOBAL_KEY]?: NodeJS.Timeout;
  [GLOBAL_KEY_FAST]?: NodeJS.Timeout;
};

/** How often the fast-catch poll runs, and how long its liveness probe waits. */
export const FAST_CATCH_INTERVAL_MS = 3_000;
export const FAST_CATCH_PROBE_TIMEOUT_MS = 2_500;

// One window delivery in flight at a time, shared by the 30s pulse and the
// fast-catch poll so they never both push the same queued frame at once.
let windowBusy = false;
async function guardedWindowTick(): Promise<void> {
  if (windowBusy) return;
  windowBusy = true;
  try {
    await runDeviceWindowTick();
  } finally {
    windowBusy = false;
  }
}

/**
 * A cheap, frequent liveness poll that catches a battery panel's brief wake.
 *
 * The 30s pulse blind-polls, and a panel that wakes for about a minute every
 * quarter hour is missed as often as not — the whole reason autonomous delivery
 * looked unreliable. This runs every few seconds but does network work ONLY
 * when a frame or a config change is actually waiting, and then only a
 * short-timeout status probe: a sleeping panel costs one ~2.5s failed connect,
 * a waking one is caught within a few seconds and handed to the full window
 * tick, which delivers with its own normal (longer) budget.
 */
export async function runFastCatchTick(): Promise<void> {
  if (windowBusy) return;
  const state = readState();
  if (state.deviceMode !== "real") return; // the mock is always reachable
  const intentWanted = readIntent() !== null;
  const hasQueued = queuedPush() !== null;
  if (!intentWanted && !hasQueued) return;

  try {
    const probe = await deviceContext({ timeoutMs: FAST_CATCH_PROBE_TIMEOUT_MS });
    if (probe.simulated) return;
    await probe.client.status();
  } catch {
    return; // asleep or unreachable — the next fast pulse tries again
  }
  // Reachable this instant: deliver on the full path before the wake closes.
  await guardedWindowTick();
}

/** Start once per persistent Node process. The short pulse only checks due state. */
export function startRefreshScheduler(): void {
  const scope = globalThis as SchedulerGlobal;
  if (scope[GLOBAL_KEY]) return;
  // Two passes, in that order, and the order is the coordination.
  //
  // The window pass first: it is a no-op unless a power intent or a frame is
  // actually waiting, it opens at most one socket when they are, and it is the
  // one with a person waiting on it. It is also the pass that DELIVERS a queued
  // frame — with the reachability check, the within-wake retries and the
  // display confirmation — so running it first means a frame that can go out
  // this window does, before the refresh pass below reconsiders it.
  //
  // The refresh pass second, and it never touches the wire: when a refresh is
  // due it RENDERS AND QUEUES the frame and lets the window pass deliver it.
  // "A refresh is due" is decoupled from "deliver when reachable" on purpose —
  // a battery panel's wake is too brief and marginal to spend on one blocking
  // push. A manual push still outstanding blocks it; its own earlier frame it
  // supersedes with the fresh render, so the queue holds at most one and the
  // panel gets current information on its next wake.
  //
  // Neither may take the other down: a device that refuses a config write must
  // not stop dashboards refreshing, and vice versa.
  const scheduler = new RefreshScheduler(async () => {
    await guardedWindowTick().catch(() => undefined);
    await runRefreshTick();
  });
  const timer = setInterval(() => { void scheduler.tick(); }, 30_000);
  timer.unref?.();
  scope[GLOBAL_KEY] = timer;
  void scheduler.tick();

  // The fast-catch poll: frequent, cheap, and the thing that actually lands a
  // frame inside a battery panel's short wake. It shares `windowBusy` with the
  // pulse above, so the two never deliver at once.
  const fastTimer = setInterval(() => {
    void runFastCatchTick().catch(() => undefined);
  }, FAST_CATCH_INTERVAL_MS);
  fastTimer.unref?.();
  scope[GLOBAL_KEY_FAST] = fastTimer;
}
