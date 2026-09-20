import { randomUUID } from "node:crypto";
import { z } from "zod";
import { appendJsonLine, readJsonLines } from "../store/atomicFile";
import { ensureDataRoot, paths } from "../store/paths";

/**
 * The write-ahead push ledger.
 *
 * Append only, one JSON object per line, fsynced before the line is considered
 * written. State transitions append a new line; a line is never rewritten.
 * That is what makes "we sent something and do not know what happened" a
 * recoverable fact rather than a lost one.
 *
 * The ledger is also the sha-to-version map: it recorded the digest of every
 * frame the tower ever sent, so a digest the device reports can be resolved
 * back to a dashboard version, or honestly labelled as an unknown frame.
 */

export type PushState =
  | "pending"
  | "sent"
  | "verified_displayed"
  | "failed"
  | "uncertain"
  | "acknowledged"
  /**
   * Rendered, recorded, and not sent: the device was not there. The bytes are
   * on disk beside this line and go out at the next window where the device
   * actually answers. See src/core/pushQueue.ts for why this is not "failed".
   */
  | "queued";

export const TERMINAL_STATES: readonly PushState[] = [
  "verified_displayed",
  "failed",
  "acknowledged",
];

/**
 * States that must block a new push until they are resolved.
 *
 * `queued` is in here for a different reason from the other three. Those are
 * unknowns: something went out and the tower cannot see what happened. A
 * queued push is perfectly known — nothing went out — but it is still an
 * outstanding promise to paint a specific frame, and letting a second push
 * stack on top of it would mean two frames racing for the same wake window
 * with no way to say which one the panel ends up showing. It is withdrawable
 * in one click, which the unknowns are not.
 */
export const BLOCKING_STATES: readonly PushState[] = [
  "pending",
  "sent",
  "uncertain",
  "queued",
];

export const LedgerLineSchema = z.object({
  schema_version: z.literal(1),
  pushId: z.string(),
  state: z.enum([
    "pending",
    "sent",
    "verified_displayed",
    "failed",
    "uncertain",
    "acknowledged",
    "queued",
  ]),
  at: z.string(),
  dashboardId: z.string(),
  dashboardTitle: z.string(),
  version: z.number().int(),
  sha256: z.string(),
  semanticHash: z.string(),
  idempotencyKey: z.string(),
  deviceMode: z.enum(["mock", "real"]),
  /**
   * Who created this push: a person ("manual") or the scheduler ("auto").
   *
   * Defaulted to "manual" so every line written before this field existed reads
   * back as the stricter kind, and so the schema version does not move — the
   * same append-only, no-migration discipline as `attempts` above. It is the
   * one bit that lets an automatic delivery relax where a manual one must not:
   * an unconfirmed automatic frame may be retried inside its bounded budget and
   * superseded by the next due render, while a manual push keeps its "an
   * uncertain outcome blocks until a person resolves it" contract.
   */
  origin: z.enum(["manual", "auto"]).default("manual"),
  forced: z.boolean().default(false),
  seq: z.number().int().nullable().default(null),
  deduped: z.boolean().nullable().default(null),
  replay: z.boolean().nullable().default(null),
  render: z.string().nullable().default(null),
  panelMs: z.number().int().nullable().default(null),
  errorCode: z.string().nullable().default(null),
  /**
   * Wire attempts that failed transiently, for a queued push.
   *
   * Defaulted rather than versioned: every line written before the queue
   * existed has no attempts and reads back as zero, so the schema version does
   * not move and no migration touches a file that is append-only by design.
   * Waiting for a sleeping device is not an attempt and never increments this;
   * see src/core/pushQueue.ts.
   */
  attempts: z.number().int().nonnegative().default(0),
  /** Human detail, always secret free. */
  detail: z.string().nullable().default(null),
});
export type LedgerLine = z.infer<typeof LedgerLineSchema>;

export type NewLedgerLine = Omit<LedgerLine, "schema_version" | "at"> & {
  at?: string;
};

export function appendLedger(line: NewLedgerLine): LedgerLine {
  ensureDataRoot();
  const full: LedgerLine = LedgerLineSchema.parse({
    schema_version: 1,
    at: line.at ?? new Date().toISOString(),
    ...line,
  });
  appendJsonLine(paths.ledger(), full);
  return full;
}

export function readLedger(): LedgerLine[] {
  ensureDataRoot();
  return readJsonLines(paths.ledger(), LedgerLineSchema);
}

export interface PushRecord {
  pushId: string;
  state: PushState;
  first: LedgerLine;
  latest: LedgerLine;
  history: LedgerLine[];
}

/** Fold the append-only lines into one current record per push. */
export function readPushes(): PushRecord[] {
  const byId = new Map<string, LedgerLine[]>();
  for (const line of readLedger()) {
    const existing = byId.get(line.pushId);
    if (existing) existing.push(line);
    else byId.set(line.pushId, [line]);
  }

  return [...byId.values()]
    .map((history) => {
      const first = history[0] as LedgerLine;
      const latest = history[history.length - 1] as LedgerLine;
      return { pushId: first.pushId, state: latest.state, first, latest, history };
    })
    .sort((a, b) => b.first.at.localeCompare(a.first.at));
}

/**
 * A pending, sent, uncertain or queued push blocks every new push until it is
 * resolved — re-checked, acknowledged, delivered or withdrawn. This mirrors
 * the composer's proven 409 discipline: never guess, never auto-retry, never
 * stack writes on a device whose state you cannot see.
 */
export function blockingPush(): PushRecord | null {
  return (
    readPushes().find((record) => BLOCKING_STATES.includes(record.state)) ?? null
  );
}

/**
 * The one push waiting for a device that was not there, if any.
 *
 * Singular by construction: `blockingPush` refuses a new push while this one
 * stands, so there is never a second. Returned as the whole record because
 * every caller needs both ends of it — `first` carries the frame's identity
 * and the clock the expiry runs on, `latest` carries the attempt count.
 */
export function queuedPush(): PushRecord | null {
  return readPushes().find((record) => record.state === "queued") ?? null;
}

export function findPush(pushId: string): PushRecord | null {
  return readPushes().find((record) => record.pushId === pushId) ?? null;
}

/**
 * Resolve a digest the device reported back to the dashboard version that
 * produced it. A digest with no ledger entry is an unknown frame and must be
 * labelled as one; guessing is exactly what this product refuses to do.
 */
export function resolveSha(
  sha256: string,
): { dashboardId: string; dashboardTitle: string; version: number } | null {
  if (!sha256) return null;
  for (const record of readPushes()) {
    if (record.first.sha256 === sha256) {
      return {
        dashboardId: record.first.dashboardId,
        dashboardTitle: record.first.dashboardTitle,
        version: record.first.version,
      };
    }
  }
  return null;
}

/** The most recent push that the device confirmed it displayed. */
export function lastVerifiedPush(): PushRecord | null {
  return (
    readPushes().find((record) => record.state === "verified_displayed") ?? null
  );
}

export function newPushId(): string {
  return randomUUID();
}

export function newIdempotencyKey(): string {
  return randomUUID();
}
