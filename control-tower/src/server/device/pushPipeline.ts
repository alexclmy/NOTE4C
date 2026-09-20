import fs from "node:fs";
import { pack } from "@/core/frame";
import type { DashboardDoc } from "@/core/model";
import { renderDashboard } from "@/core/render";
import type { DashboardSources } from "@/core/render/data";
import { PANEL_TIMEZONE, SEMANTIC_SENTINEL } from "@/core/render";
import { REAL_PUSH_CONFIRMATION } from "@/core/gates";
import {
  autoQueuedRetryDelayMs,
  isTransientPushCode,
  planQueuedPush,
  type QueuedPushFacts,
} from "@/core/pushQueue";
import { sha256HexSync } from "../hash";
import { appendAudit } from "../audit";
import { writeFileAtomic } from "../store/atomicFile";
import { ensureDataRoot, paths } from "../store/paths";
import {
  DeviceClient,
  DeviceError,
  DeviceUncertainError,
  type DeviceStatus,
} from "./client";
import {
  appendLedger,
  blockingPush,
  findPush,
  lastVerifiedPush,
  newIdempotencyKey,
  newPushId,
  queuedPush,
  type LedgerLine,
  type NewLedgerLine,
  type PushRecord,
} from "./ledger";

/**
 * Poll schedule, in milliseconds after the PUT was accepted.
 *
 * 2, 5 and 10 seconds catch a fast panel; then every 10 seconds to 90 covers
 * the observed ~26.5 s refresh with room to spare; then every 30 seconds to
 * 390 seconds, which is the firmware's own worst case (read_busy waits up to
 * 120 s and EPD_TurnOnDisplay calls it three times).
 */
export function pollSchedule(): number[] {
  const points = [2_000, 5_000, 10_000];
  for (let t = 20_000; t <= 90_000; t += 10_000) points.push(t);
  for (let t = 120_000; t <= 390_000; t += 30_000) points.push(t);
  return points;
}

export const WORST_CASE_ACK_MS = 390_000;

export { REAL_PUSH_CONFIRMATION };

/**
 * How long to keep asking the device what it is showing before recording the
 * outcome as uncertain. Defaults to the firmware's own worst case; the browser
 * suite shortens it so a deliberate never-acknowledged push does not spend six
 * and a half minutes proving a point it makes in twelve seconds.
 */
export function pollBudgetMs(): number {
  const override = Number(process.env.NOTE4C_PUSH_MAX_WAIT_MS ?? "");
  return Number.isFinite(override) && override > 0 ? override : WORST_CASE_ACK_MS;
}

/**
 * Per-attempt deadlines for an AUTOMATIC delivery, sized for a battery panel's
 * ~90 s wake rather than the firmware's absolute worst case.
 *
 * A manual push is a person watching a modal, so it waits out the firmware's
 * full render budget and reports `uncertain` honestly when it runs out. An
 * automatic delivery is one of several the window pass will make inside a single
 * wake, so each attempt has to fail fast enough to leave room for the next:
 *
 *  - the PUT deadline is short, so a write that hangs (connection accepted, no
 *    answer) is abandoned in twenty seconds instead of the sixty a manual
 *    upload allows — the exact failure that was burning a whole wake;
 *  - the display poll gives the ~26.5 s render one wake's worth of time and no
 *    more, then hands the frame back to the queue to be re-checked on the next
 *    wake rather than sitting on the device mutex for six minutes.
 *
 * Both are overridable per call so the suite does not wait them out.
 */
export const AUTO_DELIVERY_UPLOAD_TIMEOUT_MS = 20_000;
export const AUTO_DELIVERY_POLL_BUDGET_MS = 35_000;

export type PushOutcome =
  | "verified_displayed"
  | "uncertain"
  | "failed"
  | "would_dedup"
  | "blocked"
  /**
   * Rendered, recorded, not sent: nobody was home. The bytes are on disk and
   * go out at the next window where the device answers.
   */
  | "queued";

export interface PushRequest {
  doc: DashboardDoc;
  version: number;
  sources: DashboardSources;
  client: DeviceClient;
  deviceMode: "mock" | "real";
  /** Push even though the semantic hash matches the last verified push. */
  force?: boolean;
  now?: Date;
  /** Injected for tests so the suite does not sit through real poll delays. */
  sleep?: (ms: number) => Promise<void>;
  /** Injected for tests: an upper bound on wall-clock waiting. */
  maxWaitMs?: number;
  /**
   * Hold the frame instead of failing it when the device is not there.
   *
   * Off by default, and the default is the point: this changes what a failed
   * write *means*, so every caller says out loud whether it wants it. Both real
   * callers do. The push route does because a person asked for this frame and a
   * sleeping device is not a refusal. The automatic refresh does because a
   * device in `auto_saver` is asleep almost all the time, so a direct push that
   * only lands when the due tick happens to fall inside a wake window would
   * almost never reach the panel — the frame is held and delivered on the next
   * wake instead. The staleness that would otherwise argue against queuing an
   * automatic refresh is handled where it belongs: the scheduler supersedes a
   * still-queued frame with a fresher render each interval (see
   * `runRefreshTick`), and `QUEUED_PUSH_MAX_AGE_MS` drops one that has aged out.
   */
  queueWhenUnreachable?: boolean;
  /**
   * Who is asking. Defaults to "manual": `push` is the person-initiated wire
   * path, and a person's unconfirmed push keeps the strict "uncertain blocks
   * until you resolve it" contract. The scheduler's own enqueue records "auto",
   * which is what lets a background delivery retry within its bounded budget and
   * be superseded by the next due render instead of wedging.
   */
  origin?: "manual" | "auto";
}

export interface PushResult {
  outcome: PushOutcome;
  pushId: string | null;
  sha256: string;
  semanticHash: string;
  seq: number | null;
  deduped: boolean;
  replay: boolean;
  panelMs: number | null;
  detail: string;
  /** Set when a prior unresolved push refused this one. */
  blockedBy?: PushRecord;
}

function defaultSleep(ms: number): Promise<void> {
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, ms);
    timer.unref?.();
  });
}

/** Render the frame and both digests without touching the network. */
export function prepareFrame(
  doc: DashboardDoc,
  sources: DashboardSources,
  now: Date,
): { bytes: Uint8Array; sha256: string; semanticHash: string } {
  const bytes = pack(
    renderDashboard(doc, sources, { now, timeZone: PANEL_TIMEZONE }),
  );
  const semanticBytes = pack(
    renderDashboard(doc, sources, {
      now: SEMANTIC_SENTINEL,
      timeZone: PANEL_TIMEZONE,
    }),
  );
  return {
    bytes,
    sha256: sha256HexSync(bytes),
    semanticHash: sha256HexSync(semanticBytes),
  };
}

/**
 * Ask the device what it is showing, on the schedule above, until it says our
 * digest or the budget runs out.
 *
 * Shared by the direct push and by the delivery of a queued one, so that a
 * frame verified an hour after it was requested is verified by exactly the
 * same rule as one verified immediately: the device's own displayed digest,
 * never an inference from a 202.
 *
 * Returns the last status it managed to read and, when the frame was
 * confirmed, how long the panel took. `panelMs === null` means it was not
 * confirmed inside the budget, which is uncertain rather than failed.
 */
async function pollForDisplayed(options: {
  client: DeviceClient;
  sha256: string;
  sleep: (ms: number) => Promise<void>;
  budgetMs: number;
}): Promise<{ status: DeviceStatus | null; panelMs: number | null }> {
  const started = Date.now();
  let status: DeviceStatus | null = null;
  let waited = 0;

  for (const at of pollSchedule()) {
    if (at > options.budgetMs) break;
    await options.sleep(at - waited);
    waited = at;

    try {
      status = await options.client.status();
    } catch {
      // A single unreadable status is not a verdict. Keep polling; the budget
      // is what ends this loop.
      continue;
    }

    if (status.displayed.sha256 === options.sha256) {
      const panelMs =
        typeof status.timing_ms?.panel === "number"
          ? Math.round(status.timing_ms.panel)
          : Date.now() - started;
      return { status, panelMs };
    }
  }

  return { status, panelMs: null };
}

// ------------------------------------------------------------- the queue ---

/**
 * Keep the exact bytes of a push that could not be delivered.
 *
 * Written before the ledger line that refers to them, and fsynced by
 * `writeFileAtomic`, so a ledger that says `queued` is never a ledger pointing
 * at a file that is not there yet.
 */
export function storeQueuedFrame(pushId: string, bytes: Uint8Array): void {
  ensureDataRoot();
  writeFileAtomic(paths.queuedFrame(pushId), Buffer.from(bytes));
}

export function readQueuedFrame(pushId: string): Uint8Array | null {
  try {
    return new Uint8Array(fs.readFileSync(paths.queuedFrame(pushId)));
  } catch {
    return null;
  }
}

/** Forget the bytes. Called the moment a push reaches any terminal state. */
export function dropQueuedFrame(pushId: string): void {
  try {
    fs.unlinkSync(paths.queuedFrame(pushId));
  } catch {
    // Already gone, which is the state this function exists to produce.
  }
}

/**
 * What the queue policy needs to know, read off the append-only ledger.
 *
 * `first` is the original request, and it is the clock the expiry runs on.
 * `latest` is the most recent line, which for a queued push is either that
 * same line or the record of a wire attempt that did not land.
 */
export function queuedFactsFrom(record: PushRecord): QueuedPushFacts {
  const attempts = record.latest.attempts;
  return {
    queuedAt: record.first.at,
    attempts,
    lastAttemptAt: attempts > 0 ? record.latest.at : null,
    lastError: attempts > 0 ? record.latest.detail : null,
  };
}

/** Rebuild a ledger line for an existing push, carrying its identity forward. */
function lineFor(
  record: PushRecord,
  overrides: Partial<NewLedgerLine> & { state: LedgerLine["state"] },
): NewLedgerLine {
  const base = record.first;
  return {
    pushId: base.pushId,
    dashboardId: base.dashboardId,
    dashboardTitle: base.dashboardTitle,
    version: base.version,
    sha256: base.sha256,
    semanticHash: base.semanticHash,
    idempotencyKey: base.idempotencyKey,
    deviceMode: base.deviceMode,
    origin: base.origin,
    forced: base.forced,
    seq: record.latest.seq,
    deduped: record.latest.deduped,
    replay: record.latest.replay,
    render: record.latest.render,
    panelMs: null,
    errorCode: null,
    attempts: record.latest.attempts,
    detail: null,
    ...overrides,
  };
}

export type QueuedDeliveryOutcome =
  /** Nothing is queued. The overwhelmingly common case; costs one file read. */
  | "none"
  /** The device is not answering. Nothing was written, on either side. */
  | "waiting"
  /** The device is here, but a failed attempt's backoff has not run out. */
  | "backoff"
  /** Dropped: too old, or too many attempts. Bounded, and recorded. */
  | "expired"
  /** The bytes went out and the device confirmed the panel is showing them. */
  | "verified_displayed"
  /** The bytes went out and the panel never confirmed. Never auto-retried. */
  | "uncertain"
  /** The device answered and said no. Terminal, and visible. */
  | "failed"
  /** An attempt did not land for a reason that may pass. Still queued. */
  | "retry_queued"
  /**
   * The tower is pointed at a different device than the one this frame was
   * queued for. Nothing is written and nothing is sent.
   */
  | "mode_changed";

export interface QueuedDeliveryResult {
  outcome: QueuedDeliveryOutcome;
  pushId: string | null;
  detail: string;
  /** True when bytes actually went on the wire during this call. */
  attempted: boolean;
}

/**
 * Deliver the one queued push, if this is a window where that is possible.
 *
 * Called from the scheduler's device pass and from the explicit "send it now"
 * action, and both go through `planQueuedPush`, so the sentence the interface
 * shows and the action the tower takes cannot disagree.
 *
 * @param status the status the caller has already read. Passed in rather than
 *        read again: the device serves four sockets and runs its own interface
 *        off the same server, so the power intent and the queued frame share
 *        one read of it per window.
 */
export async function deliverQueuedPush(options: {
  client: DeviceClient;
  status: DeviceStatus | null;
  reachable: boolean;
  /** Which device the tower is pointed at *now*. */
  deviceMode: "mock" | "real";
  now?: Date;
  sleep?: (ms: number) => Promise<void>;
  maxWaitMs?: number;
  /** Per-attempt PUT deadline. Defaults per origin; overridable for tests. */
  uploadTimeoutMs?: number;
}): Promise<QueuedDeliveryResult> {
  const now = options.now ?? new Date();
  const sleep = options.sleep ?? defaultSleep;

  const record = queuedPush();
  if (record === null) {
    return { outcome: "none", pushId: null, detail: "", attempted: false };
  }

  // An automatic frame the scheduler owns is delivered under a relaxed contract
  // built for a battery panel's brief, marginal wake: short per-attempt
  // deadlines, a fast flat retry so several attempts fit in one wake, and — the
  // point of it — an unconfirmed attempt is HELD for another try rather than
  // surrendered to a terminal `uncertain` that wedges every later due frame.
  // A manual frame keeps the strict contract: one attempt, and an unconfirmed
  // outcome is `uncertain` until a person resolves it.
  const auto = record.first.origin === "auto";

  // The tower was pointed somewhere else after this was queued. Switching to
  // the mock is not gated on an unresolved push — going back to the simulator
  // is the safe direction — but delivering a frame that was rendered for, and
  // recorded against, the real panel to the mock instead would put a line in
  // the ledger saying a real push was displayed when no hardware was involved.
  // Held, silently, exactly like a device that is not there: point the tower
  // back at the panel to deliver it, or withdraw it.
  if (record.first.deviceMode !== options.deviceMode) {
    return {
      outcome: "mode_changed",
      pushId: record.pushId,
      detail: `This frame was queued for the ${record.first.dashboardTitle} dashboard while the tower was on the ${record.first.deviceMode} device, and the tower is now on the ${options.deviceMode} one. Nothing was sent. Switch back to deliver it, or withdraw it.`,
      attempted: false,
    };
  }

  const target = `${record.first.dashboardId}@v${record.first.version}`;
  const plan = planQueuedPush({
    queued: queuedFactsFrom(record),
    reachable: options.reachable,
    now,
    retryDelayFor: auto ? autoQueuedRetryDelayMs : undefined,
  });

  if (plan.kind === "waiting" || plan.kind === "backoff") {
    // Nothing is written here, and that is the whole of it. The scheduler
    // comes round every thirty seconds whether or not the device is there;
    // a line per pass would turn one patient wait into a hundred and twenty
    // entries an hour saying nothing happened, and would make `attempts`
    // count the clock rather than the wire.
    return {
      outcome: plan.kind,
      pushId: record.pushId,
      detail: plan.reason,
      attempted: false,
    };
  }

  if (plan.kind === "expired") {
    appendLedger(
      lineFor(record, {
        state: "failed",
        errorCode: "queue_expired",
        detail: plan.reason.slice(0, 400),
        at: now.toISOString(),
      }),
    );
    dropQueuedFrame(record.pushId);
    appendAudit({
      action: "device.push.queue.expired",
      target,
      params: { sha256: record.first.sha256, attempts: record.latest.attempts },
      outcome: "failed",
      deviceConfirmed: false,
      detail: plan.reason,
    });
    return {
      outcome: "expired",
      pushId: record.pushId,
      detail: plan.reason,
      attempted: false,
    };
  }

  // plan.kind === "send". The device is here.

  // Already on the glass. That happens when an earlier attempt landed and the
  // answer never made it back, and it is the reason this check comes before
  // the write rather than after it: re-sending would be a second panel refresh
  // for a frame that is already displayed.
  if (options.status && options.status.displayed.sha256 === record.first.sha256) {
    const detail = `The device is already displaying this frame at seq ${options.status.displayed.seq}; nothing needed sending.`;
    appendLedger(
      lineFor(record, {
        state: "verified_displayed",
        seq: options.status.displayed.seq,
        detail,
        at: now.toISOString(),
      }),
    );
    dropQueuedFrame(record.pushId);
    appendAudit({
      action: "device.push.deliver",
      target,
      params: { sha256: record.first.sha256, seq: options.status.displayed.seq },
      outcome: "ok",
      deviceConfirmed: true,
      detail,
    });
    return {
      outcome: "verified_displayed",
      pushId: record.pushId,
      detail,
      attempted: false,
    };
  }

  const bytes = readQueuedFrame(record.pushId);
  if (bytes === null) {
    // The bytes are gone and this push's identity is its digest, so there is
    // nothing honest to send under it. Re-rendering now would produce
    // different pixels from the ones the ledger recorded, which would make
    // every later verification compare against a frame that never existed.
    const detail =
      "The queued frame is missing from the tower's own store, so there is nothing to send. Nothing reached the device. Push the dashboard again.";
    appendLedger(
      lineFor(record, {
        state: "failed",
        errorCode: "frame_missing",
        detail,
        at: now.toISOString(),
      }),
    );
    appendAudit({
      action: "device.push.deliver",
      target,
      params: { sha256: record.first.sha256 },
      outcome: "failed",
      deviceConfirmed: false,
      detail,
    });
    return { outcome: "failed", pushId: record.pushId, detail, attempted: false };
  }

  // Exactly one outbound write per attempt, carrying the SAME idempotency key
  // the push was recorded with. The device's own ring collapses a repeat into
  // a replay, so an attempt whose answer was lost cannot paint the panel
  // twice. The epoch is the moment the frame was rendered, not now: it is a
  // fact about the content, and lying about it would tell the device this is
  // fresher than it is.
  let accepted;
  try {
    accepted = await options.client.putFrame(bytes, {
      sha256: record.first.sha256,
      idempotencyKey: record.first.idempotencyKey,
      epochSeconds: Math.floor(new Date(record.first.at).getTime() / 1000),
      timeoutMs:
        options.uploadTimeoutMs ??
        (auto ? AUTO_DELIVERY_UPLOAD_TIMEOUT_MS : undefined),
    });
  } catch (error) {
    if (error instanceof DeviceUncertainError) {
      if (auto) {
        // A marginal wake: the connection was accepted and then the PUT never
        // answered before the short automatic deadline. The bytes may or may
        // not have landed — but this attempt carried the frame's FIXED
        // idempotency key, so re-sending is a replay on the device's own ring,
        // never a second repaint, and the "already displaying" check above
        // catches the case where it did land. So the frame is HELD, not
        // surrendered to `uncertain`: the window pass tries again on a later
        // tick within the bounded attempt budget, and a genuinely dead link
        // simply exhausts that budget and expires. This is the wedge the
        // battery path exists to avoid.
        const attempts = record.latest.attempts + 1;
        const detail = `Attempt ${attempts} did not confirm within the wake: ${error.message}`;
        appendLedger(
          lineFor(record, {
            state: "queued",
            errorCode: "uncertain",
            attempts,
            detail,
            at: now.toISOString(),
          }),
        );
        appendAudit({
          action: "device.push.deliver",
          target,
          params: { sha256: record.first.sha256, attempts },
          outcome: "pending",
          deviceConfirmed: false,
          detail,
        });
        return {
          outcome: "retry_queued",
          pushId: record.pushId,
          detail,
          attempted: true,
        };
      }

      // Manual: the write may or may not have landed, and it is never retried
      // automatically — that is the whole reason `uncertain` is a state and not
      // a retry. It blocks until a person resolves it.
      appendLedger(
        lineFor(record, {
          state: "uncertain",
          errorCode: "uncertain",
          detail: error.message,
          at: now.toISOString(),
        }),
      );
      dropQueuedFrame(record.pushId);
      appendAudit({
        action: "device.push.deliver",
        target,
        params: { sha256: record.first.sha256 },
        outcome: "uncertain",
        deviceConfirmed: false,
        detail: error.message,
      });
      return {
        outcome: "uncertain",
        pushId: record.pushId,
        detail: error.message,
        attempted: true,
      };
    }

    const deviceError =
      error instanceof DeviceError
        ? error
        : new DeviceError("unknown", "The push failed for an unknown reason");

    if (isTransientPushCode(deviceError.code)) {
      // It went back to sleep in the gap between the status read and the
      // write, or it is busy with its own refresh. Still queued, one attempt
      // spent, and the backoff decides when the next one may happen.
      const attempts = record.latest.attempts + 1;
      const detail = `Attempt ${attempts} did not land: ${deviceError.message}`;
      appendLedger(
        lineFor(record, {
          state: "queued",
          errorCode: deviceError.code,
          attempts,
          detail,
          at: now.toISOString(),
        }),
      );
      appendAudit({
        action: "device.push.deliver",
        target,
        params: { sha256: record.first.sha256, attempts },
        outcome: "pending",
        deviceConfirmed: false,
        detail,
      });
      return {
        outcome: "retry_queued",
        pushId: record.pushId,
        detail,
        attempted: true,
      };
    }

    // The device answered and refused. Terminal, visible, and not retried:
    // a wrong token or a refused digest does not become right by being sent
    // again at the next wake.
    const detail = `${deviceError.code}: ${deviceError.message}`;
    appendLedger(
      lineFor(record, {
        state: "failed",
        errorCode: deviceError.code,
        detail: deviceError.message,
        at: now.toISOString(),
      }),
    );
    dropQueuedFrame(record.pushId);
    appendAudit({
      action: "device.push.deliver",
      target,
      params: { sha256: record.first.sha256 },
      outcome: "failed",
      deviceConfirmed: false,
      detail,
    });
    return { outcome: "failed", pushId: record.pushId, detail, attempted: true };
  }

  // The device accepted the bytes. That is not the same as displaying them.
  appendLedger(
    lineFor(record, {
      state: "sent",
      seq: accepted.seq ?? null,
      deduped: accepted.deduped ?? false,
      replay: accepted.replay ?? false,
      render: accepted.render ?? null,
      detail: "Sent from the queue",
      at: now.toISOString(),
    }),
  );

  const budget =
    options.maxWaitMs ?? (auto ? AUTO_DELIVERY_POLL_BUDGET_MS : pollBudgetMs());
  const { status: lastStatus, panelMs } = await pollForDisplayed({
    client: options.client,
    sha256: record.first.sha256,
    sleep,
    budgetMs: budget,
  });

  if (panelMs !== null && lastStatus !== null) {
    // Confirmed. The bytes are now a delivered fact, so the tower stops holding
    // them.
    dropQueuedFrame(record.pushId);
    const detail = `Delivered from the queue and displayed at seq ${lastStatus.displayed.seq}, panel ${panelMs} ms${
      accepted.deduped ? ", the device deduped identical bytes" : ""
    }`;
    appendLedger(
      lineFor(record, {
        state: "verified_displayed",
        seq: lastStatus.displayed.seq,
        deduped: accepted.deduped ?? false,
        replay: accepted.replay ?? false,
        render: accepted.render ?? null,
        panelMs,
        detail,
      }),
    );
    appendAudit({
      action: "device.push.deliver",
      target,
      params: {
        sha256: record.first.sha256,
        seq: lastStatus.displayed.seq,
        deduped: accepted.deduped ?? false,
      },
      outcome: "ok",
      deviceConfirmed: true,
      detail,
    });
    return {
      outcome: "verified_displayed",
      pushId: record.pushId,
      detail,
      attempted: true,
    };
  }

  if (auto) {
    // Accepted (the 202 is already on the ledger as `sent`) but the panel did
    // not report it displayed inside one wake's poll — it is rendering, or it
    // slept mid-render. Held, not surrendered: the bytes are on the device now,
    // so the next window's status read either finds them on the glass (the
    // "already displaying" short-circuit above, which resolves it to verified
    // with no second write) or replays them under the same key. Counted as an
    // attempt so the retry budget stays bounded and a device that never renders
    // eventually expires honestly rather than being re-sent forever.
    const attempts = record.latest.attempts + 1;
    const detail = lastStatus
      ? `Attempt ${attempts} stored as seq ${lastStatus.stored.seq} but the panel did not confirm it displayed within ${Math.round(budget / 1000)} s; will re-check on the next wake`
      : `Attempt ${attempts} was sent but the device stopped answering before confirming; will re-check on the next wake`;
    appendLedger(
      lineFor(record, {
        state: "queued",
        seq: accepted.seq ?? null,
        deduped: accepted.deduped ?? false,
        replay: accepted.replay ?? false,
        render: accepted.render ?? null,
        errorCode: "not_displayed",
        attempts,
        detail,
        at: now.toISOString(),
      }),
    );
    appendAudit({
      action: "device.push.deliver",
      target,
      params: { sha256: record.first.sha256, attempts },
      outcome: "pending",
      deviceConfirmed: false,
      detail,
    });
    return { outcome: "retry_queued", pushId: record.pushId, detail, attempted: true };
  }

  // Manual: the bytes went out and were stored, but the panel never confirmed.
  // Uncertain, not failed — and the tower stops holding the bytes, because a
  // manual push is not retried automatically. It blocks until a person resolves
  // it.
  dropQueuedFrame(record.pushId);
  const detail = lastStatus
    ? `Sent from the queue and stored as seq ${lastStatus.stored.seq}, but the panel never reported it displayed within ${Math.round(budget / 1000)} s`
    : "Sent from the queue, and the device stopped answering before confirming the frame was displayed";
  appendLedger(
    lineFor(record, {
      state: "uncertain",
      seq: accepted.seq ?? null,
      deduped: accepted.deduped ?? false,
      replay: accepted.replay ?? false,
      render: accepted.render ?? null,
      errorCode: "not_displayed",
      detail,
    }),
  );
  appendAudit({
    action: "device.push.deliver",
    target,
    params: { sha256: record.first.sha256 },
    outcome: "uncertain",
    deviceConfirmed: false,
    detail,
  });
  return { outcome: "uncertain", pushId: record.pushId, detail, attempted: true };
}

export interface EnqueueRequest {
  doc: DashboardDoc;
  version: number;
  sources: DashboardSources;
  deviceMode: "mock" | "real";
  /** Queue even though the semantic hash matches the last verified push. */
  force?: boolean;
  now?: Date;
  /** Defaults to "auto": this is the scheduler's produce-and-hold path. */
  origin?: "manual" | "auto";
}

/**
 * Render a due frame and HOLD it, without ever touching the device.
 *
 * This is the automatic refresh's whole contribution now: "a frame is due" is
 * decoupled from "deliver it when the panel is reachable". On a battery device
 * the wake window is brief and marginal, so a synchronous push that lands
 * during Wi-Fi association — or a moment before the panel sleeps — hangs into
 * `uncertain` and wedges the wake. The delivery, with its reachability check,
 * its retries across the several ticks in one wake and its confirmation from the
 * device's own displayed digest, belongs to `deliverQueuedPush` and the window
 * pass.
 *
 * So this renders, applies the same semantic dedup gate a direct push does, and
 * writes a single `queued` line with the exact bytes beside it. No socket is
 * opened, nothing is written `pending` (there is no wire attempt to be
 * write-ahead for), and the outcome is only ever `queued`, `would_dedup`, or
 * `blocked` — never a device fact.
 */
export async function enqueueFrame(request: EnqueueRequest): Promise<PushResult> {
  const now = request.now ?? new Date();

  // A prior unresolved push owns the queue. The scheduler resolves its own
  // outstanding frame (it supersedes it) before calling here, so a blocker at
  // this point is a manual push in flight, which this must not stack on.
  const blocker = blockingPush();
  if (blocker) {
    return {
      outcome: "blocked",
      pushId: null,
      sha256: "",
      semanticHash: "",
      seq: null,
      deduped: false,
      replay: false,
      panelMs: null,
      detail: `A ${blocker.state} push from ${blocker.first.at} must be resolved first`,
      blockedBy: blocker,
    };
  }

  const { bytes, sha256, semanticHash } = prepareFrame(
    request.doc,
    request.sources,
    now,
  );

  // The same dedup gate as a direct push: identical visible content would only
  // cost a full panel refresh, so holding it takes an explicit Force.
  const lastVerified = lastVerifiedPush();
  if (
    !request.force &&
    lastVerified &&
    lastVerified.first.semanticHash === semanticHash
  ) {
    return {
      outcome: "would_dedup",
      pushId: null,
      sha256,
      semanticHash,
      seq: null,
      deduped: true,
      replay: false,
      panelMs: null,
      detail:
        "Unchanged since the last verified push. Holding it would queue the same picture.",
    };
  }

  const pushId = newPushId();
  const origin = request.origin ?? "auto";
  const base = {
    pushId,
    dashboardId: request.doc.id,
    dashboardTitle: request.doc.title,
    version: request.version,
    sha256,
    semanticHash,
    idempotencyKey: newIdempotencyKey(),
    deviceMode: request.deviceMode,
    origin,
    forced: request.force ?? false,
    seq: null,
    deduped: null,
    replay: null,
    render: null,
    panelMs: null,
    errorCode: null,
    attempts: 0,
    detail: null,
  } satisfies Omit<LedgerLine, "schema_version" | "at" | "state">;

  // Bytes before the line that points at them, and fsynced by
  // `storeQueuedFrame`, so a ledger that says `queued` is never a ledger
  // pointing at a frame that is not on disk yet.
  storeQueuedFrame(pushId, bytes);
  const detail =
    "Rendered and held for delivery on the next window where the device is reachable.";
  appendLedger({ ...base, state: "queued", at: now.toISOString(), detail });
  appendAudit({
    action: "device.push",
    target: `${request.doc.id}@v${request.version}`,
    params: { sha256, deviceMode: request.deviceMode, origin },
    // "pending" is the audit word for "recorded, not delivered": not ok (nothing
    // reached the device), not failed (nothing went wrong), not refused.
    outcome: "pending",
    deviceConfirmed: false,
    detail,
  });

  return {
    outcome: "queued",
    pushId,
    sha256,
    semanticHash,
    seq: null,
    deduped: false,
    replay: false,
    panelMs: null,
    detail,
  };
}

/**
 * The push sequence. Every step is ordered so that the durable record of an
 * intent exists before anything can go wrong on the wire.
 */
export async function push(request: PushRequest): Promise<PushResult> {
  const now = request.now ?? new Date();
  const sleep = request.sleep ?? defaultSleep;

  // 1. A prior unresolved push blocks everything. No stacking writes on a
  //    device whose state we cannot see.
  const blocker = blockingPush();
  if (blocker) {
    return {
      outcome: "blocked",
      pushId: null,
      sha256: "",
      semanticHash: "",
      seq: null,
      deduped: false,
      replay: false,
      panelMs: null,
      detail: `A ${blocker.state} push from ${blocker.first.at} must be resolved first`,
      blockedBy: blocker,
    };
  }

  // 2. Render and hash.
  const { bytes, sha256, semanticHash } = prepareFrame(
    request.doc,
    request.sources,
    now,
  );

  // 3. Semantic dedup gate. Identical visible content would only cost a full
  //    panel refresh cycle, so it takes an explicit Force.
  const lastVerified = lastVerifiedPush();
  if (
    !request.force &&
    lastVerified &&
    lastVerified.first.semanticHash === semanticHash
  ) {
    return {
      outcome: "would_dedup",
      pushId: null,
      sha256,
      semanticHash,
      seq: null,
      deduped: true,
      replay: false,
      panelMs: null,
      detail:
        "Unchanged since the last verified push. Pushing would repaint the same picture.",
    };
  }

  const pushId = newPushId();
  const idempotencyKey = newIdempotencyKey();
  const base = {
    pushId,
    dashboardId: request.doc.id,
    dashboardTitle: request.doc.title,
    version: request.version,
    sha256,
    semanticHash,
    idempotencyKey,
    deviceMode: request.deviceMode,
    origin: request.origin ?? "manual",
    forced: request.force ?? false,
    seq: null,
    deduped: null,
    replay: null,
    render: null,
    panelMs: null,
    errorCode: null,
    attempts: 0,
    detail: null,
  } satisfies Omit<LedgerLine, "schema_version" | "at" | "state">;

  // 4. Persist the intent and fsync BEFORE any socket opens. If the process
  //    dies mid-PUT, the pending line is still on disk and the next start
  //    blocks rather than silently sending again.
  appendLedger({ ...base, state: "pending", at: now.toISOString() });

  // 5. Exactly one outbound write per intent.
  let accepted;
  try {
    accepted = await request.client.putFrame(bytes, {
      sha256,
      idempotencyKey,
      epochSeconds: Math.floor(now.getTime() / 1000),
    });
  } catch (error) {
    if (error instanceof DeviceUncertainError) {
      // The write may or may not have landed. Never retry it automatically.
      appendLedger({
        ...base,
        state: "uncertain",
        errorCode: "uncertain",
        detail: error.message,
      });
      appendAudit({
        action: "device.push",
        target: `${request.doc.id}@v${request.version}`,
        params: { sha256, deviceMode: request.deviceMode },
        outcome: "uncertain",
        deviceConfirmed: false,
        detail: error.message,
      });
      return {
        outcome: "uncertain",
        pushId,
        sha256,
        semanticHash,
        seq: null,
        deduped: false,
        replay: false,
        panelMs: null,
        detail: error.message,
      };
    }

    const deviceError =
      error instanceof DeviceError
        ? error
        : new DeviceError("unknown", "The push failed for an unknown reason");

    // Nobody was home. Not a refusal, and not a failure: the device is in
    // automatic power saving with its radio off, which is the state it is
    // designed to spend most of its life in. Keep the bytes and the identity
    // of this push, and send them at the next window where it answers.
    //
    // Only for the two codes that mean "not now" (see TRANSIENT_PUSH_CODES): a
    // device that answered and said no has told the tower something a retry
    // cannot change, and queueing that would be the silent failure this
    // product exists to avoid.
    if (request.queueWhenUnreachable && isTransientPushCode(deviceError.code)) {
      storeQueuedFrame(pushId, bytes);
      const detail = `Held for the next time the device is reachable. Nothing was sent: ${deviceError.message}`;
      appendLedger({
        ...base,
        state: "queued",
        errorCode: deviceError.code,
        attempts: 0,
        detail,
      });
      appendAudit({
        action: "device.push",
        target: `${request.doc.id}@v${request.version}`,
        params: { sha256, deviceMode: request.deviceMode },
        // "pending" is the audit word for "recorded, not delivered". It is
        // not ok (nothing reached the device), not failed (nothing went
        // wrong) and not refused (nobody refused it).
        outcome: "pending",
        deviceConfirmed: false,
        detail,
      });
      return {
        outcome: "queued",
        pushId,
        sha256,
        semanticHash,
        seq: null,
        deduped: false,
        replay: false,
        panelMs: null,
        detail,
      };
    }

    // A DeviceError means either the request never left (unreachable, no
    // token) or the device answered with an explicit refusal. Both are clean
    // failures: nothing was stored. Only a timeout is uncertain, and that is
    // the branch above.
    appendLedger({
      ...base,
      state: "failed",
      errorCode: deviceError.code,
      detail: deviceError.message,
    });
    appendAudit({
      action: "device.push",
      target: `${request.doc.id}@v${request.version}`,
      params: { sha256, deviceMode: request.deviceMode },
      outcome: "failed",
      deviceConfirmed: false,
      detail: `${deviceError.code}: ${deviceError.message}`,
    });
    return {
      outcome: "failed",
      pushId,
      sha256,
      semanticHash,
      seq: null,
      deduped: false,
      replay: false,
      panelMs: null,
      detail: `${deviceError.code}: ${deviceError.message}`,
    };
  }

  // 6. The device accepted the bytes. That is not the same as displaying them.
  appendLedger({
    ...base,
    state: "sent",
    seq: accepted.seq ?? null,
    deduped: accepted.deduped ?? false,
    replay: accepted.replay ?? false,
    render: accepted.render ?? null,
  });

  // 7. Poll until the device's own displayed digest matches what we sent.
  const budget = request.maxWaitMs ?? pollBudgetMs();
  const { status: lastStatus, panelMs: confirmedMs } = await pollForDisplayed({
    client: request.client,
    sha256,
    sleep,
    budgetMs: budget,
  });

  if (confirmedMs !== null && lastStatus !== null) {
    const panelMs = confirmedMs;

    appendLedger({
      ...base,
      state: "verified_displayed",
      seq: lastStatus.displayed.seq,
      deduped: accepted.deduped ?? false,
      replay: accepted.replay ?? false,
      render: accepted.render ?? null,
      panelMs,
      detail: `Displayed at seq ${lastStatus.displayed.seq}, panel ${panelMs} ms${
        accepted.deduped ? ", the device deduped identical bytes" : ""
      }`,
    });
    appendAudit({
      action: "device.push",
      target: `${request.doc.id}@v${request.version}`,
      params: { sha256, deviceMode: request.deviceMode, seq: lastStatus.displayed.seq },
      outcome: "ok",
      deviceConfirmed: true,
      detail: `Displayed, panel ${panelMs} ms`,
    });

    return {
      outcome: "verified_displayed",
      pushId,
      sha256,
      semanticHash,
      seq: lastStatus.displayed.seq,
      deduped: accepted.deduped ?? false,
      replay: accepted.replay ?? false,
      panelMs,
      detail: `Verified displayed at seq ${lastStatus.displayed.seq}`,
    };
  }

  // 8. The bytes are stored but the panel never confirmed. Uncertain, not
  //    failed: the frame may well be on the glass and we simply cannot say.
  const detail = lastStatus
    ? `Stored as seq ${lastStatus.stored.seq}, but the panel never reported it displayed within ${Math.round(budget / 1000)} s`
    : `The device stopped answering before confirming the frame was displayed`;

  appendLedger({
    ...base,
    state: "uncertain",
    seq: accepted.seq ?? null,
    deduped: accepted.deduped ?? false,
    replay: accepted.replay ?? false,
    render: accepted.render ?? null,
    errorCode: "not_displayed",
    detail,
  });
  appendAudit({
    action: "device.push",
    target: `${request.doc.id}@v${request.version}`,
    params: { sha256, deviceMode: request.deviceMode },
    outcome: "uncertain",
    deviceConfirmed: false,
    detail,
  });

  return {
    outcome: "uncertain",
    pushId,
    sha256,
    semanticHash,
    seq: accepted.seq ?? null,
    deduped: accepted.deduped ?? false,
    replay: accepted.replay ?? false,
    panelMs: null,
    detail,
  };
}

/**
 * Re-check an unresolved push against the device. This is the only way a
 * pending or uncertain entry becomes verified without a new write: it asks the
 * device what it is actually showing.
 */
export async function recheck(
  pushId: string,
  client: DeviceClient,
): Promise<{ state: string; detail: string }> {
  const record = findPush(pushId);
  if (!record) return { state: "unknown", detail: "No such push in the ledger" };

  let status: DeviceStatus;
  try {
    status = await client.status();
  } catch (error) {
    const detail =
      error instanceof Error ? error.message : "The device could not be reached";
    appendAudit({
      action: "device.push.recheck",
      target: pushId,
      outcome: "failed",
      deviceConfirmed: false,
      detail,
    });
    return { state: record.state, detail };
  }

  const base = { ...record.first };

  if (status.displayed.sha256 === base.sha256) {
    appendLedger(
      lineFor(record, {
        state: "verified_displayed",
        seq: status.displayed.seq,
        detail: "Confirmed by a later status read",
      }),
    );
    appendAudit({
      action: "device.push.recheck",
      target: pushId,
      outcome: "ok",
      deviceConfirmed: true,
      detail: "Displayed digest matches",
    });
    return {
      state: "verified_displayed",
      detail: "The device is displaying this frame",
    };
  }

  const detail =
    status.stored.sha256 === base.sha256
      ? `Stored as seq ${status.stored.seq} but the panel is showing a different frame`
      : "The device is neither storing nor displaying this frame";

  appendAudit({
    action: "device.push.recheck",
    target: pushId,
    outcome: "uncertain",
    deviceConfirmed: false,
    detail,
  });
  return { state: record.state, detail };
}

/**
 * Accept that a push will never be resolved and unblock the queue. This is a
 * deliberate human act, recorded as one: the tower never quietly decides an
 * unknown outcome was fine.
 */
export function acknowledge(pushId: string, note = ""): boolean {
  const record = findPush(pushId);
  if (!record) return false;

  const wasQueued = record.state === "queued";
  appendLedger(
    lineFor(record, {
      state: "acknowledged",
      detail:
        note.slice(0, 200) ||
        (wasQueued
          ? "Withdrawn by the operator before it was ever sent"
          : "Acknowledged by the operator"),
    }),
  );
  // A withdrawn push is not going to be sent, so the tower stops holding 30000
  // bytes for it. Unconditional rather than guarded on the state: every
  // terminal state means the same thing about the frame store.
  dropQueuedFrame(pushId);

  appendAudit({
    action: wasQueued ? "device.push.cancel" : "device.push.acknowledge",
    target: pushId,
    params: { note: note.slice(0, 200) },
    outcome: "ok",
    // A withdrawal is the one resolution the device was never party to, so it
    // is recorded as involving no device at all rather than as an unconfirmed
    // one.
    deviceConfirmed: wasQueued ? null : false,
    detail: `Previous state was ${record.state}`,
  });
  return true;
}
