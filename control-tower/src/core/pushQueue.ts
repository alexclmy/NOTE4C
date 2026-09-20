/**
 * Holding a frame for a device that was not there to receive it.
 *
 * This file is portable: no Node, no fetch, no filesystem. Everything here is
 * a pure function of its arguments, so the suite drives the awkward states
 * directly instead of waiting on a device that is, by design, asleep most of
 * the time. It is the frame-side twin of the intent logic in src/core/power.ts
 * and it obeys the same one rule:
 *
 * **A sleeping device cannot be woken over Wi-Fi.** Nothing here retries at a
 * sleeping panel, nothing here claims a push woke one, and nothing here counts
 * a wait as an attempt.
 *
 * What this exists to stop
 * -----------------------
 * A push to a device in automatic power saving used to be written to the
 * ledger as `failed / unreachable` and then thrown away. The user's request
 * was gone: the frame was rendered, the intent was recorded, the socket found
 * nobody home, and the tower's answer was a red badge. That is not a failure —
 * nobody refused anything — and the device *will* be reachable, on its own
 * schedule, usually within the hour.
 *
 * So an unreachable push becomes `queued`: the exact bytes are kept, and the
 * next window where the device actually answers, they go out. This module is
 * the decision about whether that window has arrived, and the sentences the
 * interface shows while it has not.
 *
 * The distinctions that matter
 * ----------------------------
 *  - **Waiting is not an attempt.** The scheduler comes round every thirty
 *    seconds whether or not the device is there. Counting those would drive a
 *    queued push to its attempt ceiling in four minutes of nobody being home.
 *    `attempts` counts occasions where bytes were actually put on the wire.
 *  - **Nobody home is not a refusal.** Only a transient wire failure keeps a
 *    push queued. A device that answers and says no — bad token, wrong digest,
 *    no hybrid contract, not provisioned — is a failure, recorded as one,
 *    immediately. A queue that swallowed those would be a silent one.
 *  - **A frame is a photograph of a moment.** It is not a durable preference,
 *    so it expires. Painting six-hour-old weather onto a panel because that is
 *    when the device finally woke up would be the dishonesty this product
 *    exists to avoid, dressed up as reliability.
 */

/**
 * How long a queued frame is worth delivering. Six hours.
 *
 * Chosen against what the frame *contains*, not against how long the tower
 * could stand to hold it. Every module on a panel is about now: the forecast
 * for this afternoon, the next event, the reminders still open. Six hours
 * covers a handful of `auto_saver` wake cycles at the default hour interval,
 * an evening, and a device somebody unplugged while they cooked dinner. Past
 * that the honest move is to drop it and say so — the dashboard is still
 * selected, the scheduler is still running, and the next refresh renders the
 * world as it actually is then.
 */
export const QUEUED_PUSH_MAX_AGE_MS = 6 * 60 * 60 * 1000;

/**
 * How many *wire attempts* a queued push gets before the tower stops trying.
 *
 * Five, and they are only spent on transient failures: the device answered the
 * status read and then the write did not land — it went back to sleep in the
 * gap, or it was busy with its own refresh. A device that is simply not there
 * costs no attempts at all, so this ceiling is about a device that is flapping,
 * not about one that is asleep. Five failures spread over the backoff below is
 * a little over an hour of that, at which point the answer is not going to
 * change by trying a sixth time.
 */
export const MAX_QUEUED_PUSH_ATTEMPTS = 5;

/**
 * The first retry delay, and the ceiling.
 *
 * A minute, doubling, capped at thirty. The cap is about the shortest
 * realistic wake interval: past that the tower would be retrying more often
 * than the device is even awake, which is a router getting warm rather than a
 * panel getting painted.
 */
export const QUEUED_RETRY_BASE_MS = 60_000;
export const QUEUED_RETRY_MAX_MS = 30 * 60_000;

/**
 * The device failures that leave a push queued rather than failing it.
 *
 * Deliberately a closed list of two, and deliberately not "anything that is
 * not obviously permanent". `unreachable` means the request never left this
 * machine; `busy` means the device is already serving another mutating request
 * and told us so in the one answer it gives for that. Both are "not now".
 *
 * Everything else — `unauthorized`, `not_provisioned`, `locked_out`,
 * `sha_mismatch`, `bad_length`, `lockdown`, `no_token`, any bare HTTP status —
 * is a device or a tower saying no. Those fail, visibly, at once. A queue that
 * quietly retried an authentication failure would turn a five-second fix into
 * an hour of a spinner.
 */
export const TRANSIENT_PUSH_CODES: readonly string[] = ["unreachable", "busy"];

export function isTransientPushCode(code: string): boolean {
  return TRANSIENT_PUSH_CODES.includes(code);
}

/** How long to wait after `attempts` failed wire attempts. Bounded doubling. */
export function queuedRetryDelayMs(attempts: number): number {
  if (attempts <= 0) return 0;
  const exponent = Math.min(attempts - 1, 30);
  return Math.min(QUEUED_RETRY_BASE_MS * 2 ** exponent, QUEUED_RETRY_MAX_MS);
}

/**
 * The retry cadence for an AUTOMATIC delivery, far shorter than the manual one.
 *
 * A battery panel's wake is about ninety seconds, and the whole point of the
 * automatic path is to fit two or three delivery attempts inside one wake
 * rather than burn it on a single hung write. So the window pass retries a
 * still-held automatic frame every ten seconds while the device is reachable,
 * flat, instead of the minute-and-doubling a manual queue uses — the manual
 * backoff is sized to stop hammering a flapping device over an hour, which is
 * the opposite of what a brief, precious wake window needs.
 *
 * Still bounded by MAX_QUEUED_PUSH_ATTEMPTS, and a device that is simply asleep
 * still costs no attempts at all: that is `waiting`, decided below, not a retry.
 */
export const AUTO_QUEUED_RETRY_MS = 10_000;
export function autoQueuedRetryDelayMs(attempts: number): number {
  return attempts <= 0 ? 0 : AUTO_QUEUED_RETRY_MS;
}

/** What the tower knows about the one queued push, from the ledger. */
export interface QueuedPushFacts {
  /** ISO time of the original request. The clock the expiry runs on. */
  queuedAt: string;
  /** Wire attempts that failed transiently. Waiting is not one. */
  attempts: number;
  /** ISO time of the last wire attempt, or null if there has been none. */
  lastAttemptAt: string | null;
  /** What the last attempt said, or null. Always secret free. */
  lastError: string | null;
}

export type QueuedDisposition =
  /** Nothing is queued. */
  | { kind: "none" }
  /** The device is here and the backoff is clear: send these bytes now. */
  | { kind: "send" }
  /** The device is not answering. Keep the bytes; say so; write nothing. */
  | { kind: "waiting"; reason: string }
  /** The device is here but the last write failed and the delay stands. */
  | { kind: "backoff"; retryAt: string; reason: string }
  /** Give up, with a reason a person can read. */
  | { kind: "expired"; reason: string };

/** When a queued push stops being worth delivering. */
export function queuedPushExpiresAt(queuedAt: string): Date | null {
  const at = new Date(queuedAt).getTime();
  if (!Number.isFinite(at)) return null;
  return new Date(at + QUEUED_PUSH_MAX_AGE_MS);
}

/**
 * When a queued push may next be put on the wire, or null if it may now.
 *
 * `retryDelayFor` is the cadence: the manual doubling by default, or the flat
 * automatic one the scheduler passes for a frame it owns.
 */
export function queuedRetryAt(
  queued: QueuedPushFacts,
  retryDelayFor: (attempts: number) => number = queuedRetryDelayMs,
): Date | null {
  if (queued.attempts <= 0 || queued.lastAttemptAt === null) return null;
  const last = new Date(queued.lastAttemptAt).getTime();
  if (!Number.isFinite(last)) return null;
  return new Date(last + retryDelayFor(queued.attempts));
}

/**
 * Why this queued push should be dropped, or null to keep it.
 *
 * Two ways out, and they are different failures, so they say different things.
 * Age means the device was never there; attempts means it was there and the
 * write kept not landing.
 */
export function queuedExpiryReason(
  queued: QueuedPushFacts,
  now: Date,
): string | null {
  if (queued.attempts >= MAX_QUEUED_PUSH_ATTEMPTS) {
    return `The tower tried to send this frame ${queued.attempts} times and it did not land, so it stopped rather than keep writing to a device that is not taking it. ${
      queued.lastError ?? "No reason was reported."
    }`;
  }

  const expiresAt = queuedPushExpiresAt(queued.queuedAt);
  if (expiresAt === null || now.getTime() <= expiresAt.getTime()) return null;

  const hours = Math.round(QUEUED_PUSH_MAX_AGE_MS / 3_600_000);
  return `The device was not reachable within ${hours} hours, so this frame was dropped rather than painted onto the panel hours out of date. Nothing was sent. The dashboard is unchanged; push it again, or let the next scheduled refresh render it fresh.`;
}

export interface PlanQueuedInput {
  queued: QueuedPushFacts | null;
  /** Did the read that preceded this call reach the device? */
  reachable: boolean;
  now: Date;
  /**
   * The retry cadence to apply between wire attempts. The manual doubling by
   * default; the scheduler passes `autoQueuedRetryDelayMs` for a frame it owns,
   * so several attempts fit inside one battery wake.
   */
  retryDelayFor?: (attempts: number) => number;
}

/**
 * The single decision every caller shares.
 *
 * The scheduler calls it to decide whether to write, and the interface calls
 * it to describe what is about to happen. One function, so the sentence a user
 * reads and the action the tower takes cannot disagree — the same reason
 * `planIntent` exists next door.
 */
export function planQueuedPush(input: PlanQueuedInput): QueuedDisposition {
  const { queued, reachable, now } = input;
  if (queued === null) return { kind: "none" };

  const expiry = queuedExpiryReason(queued, now);
  if (expiry !== null) return { kind: "expired", reason: expiry };

  if (!reachable) {
    return {
      kind: "waiting",
      reason:
        "The device is not answering, which is what a sleeping device looks like. The frame is held here and goes out the next time the device is actually reachable.",
    };
  }

  const retryAt = queuedRetryAt(queued, input.retryDelayFor);
  if (retryAt !== null && now.getTime() < retryAt.getTime()) {
    const seconds = Math.ceil((retryAt.getTime() - now.getTime()) / 1000);
    const waitFor =
      seconds >= 120 ? `${Math.ceil(seconds / 60)} minutes` : `${seconds} seconds`;
    return {
      kind: "backoff",
      retryAt: retryAt.toISOString(),
      reason: `The last attempt to send this frame did not land, so the tower is waiting about ${waitFor} before trying again rather than re-sending it every thirty seconds. ${
        queued.lastError ?? ""
      }`.trim(),
    };
  }

  return { kind: "send" };
}

/**
 * One honest paragraph about a queued push, for the interface.
 *
 * Built here rather than in the component so that Overview, the Device page
 * and any future surface say the same thing, and so the wording is testable
 * without a browser. It never says the tower will wake the device, and it
 * never promises a delivery time: `nextWakeLabel` is passed in already marked
 * as an estimate by the caller that computed it.
 */
export function describeQueuedPush(input: {
  queued: QueuedPushFacts;
  /** null when the tower has not read the device since this was queued. */
  reachable: boolean | null;
  deviceMode: "mock" | "real";
  /** Already formatted, and already carrying "(estimated)" where it applies. */
  nextWakeLabel: string | null;
  /** The device's own note about there being no remote wake. */
  noRemoteWakeNote: string;
  now?: Date;
}): string {
  const now = input.now ?? new Date();
  const parts: string[] = [];

  if (input.reachable === true) {
    parts.push("The device is answering, so this frame goes out on the next pass.");
  } else if (input.deviceMode === "mock") {
    // A mock has no sleep to be in, so the sentence about wake schedules would
    // be a lie about a program that is simply not running.
    parts.push(
      "The simulated device is not answering. The frame is held here and goes out as soon as it answers again; the mock does not sleep, so silence from it means it is not running.",
    );
  } else {
    parts.push(
      "The device is not answering, which is the normal state between refreshes. The frame is held here and goes out the next time the device is actually reachable.",
    );
    if (input.nextWakeLabel) {
      parts.push(`It is next expected to be reachable ${input.nextWakeLabel}.`);
    }
    parts.push(input.noRemoteWakeNote);
  }

  if (input.queued.attempts > 0) {
    parts.push(
      `${input.queued.attempts} attempt${
        input.queued.attempts === 1 ? "" : "s"
      } did not land so far. ${input.queued.lastError ?? ""}`.trim(),
    );
  }

  const expiresAt = queuedPushExpiresAt(input.queued.queuedAt);
  if (expiresAt !== null) {
    const hoursLeft = (expiresAt.getTime() - now.getTime()) / 3_600_000;
    parts.push(
      hoursLeft <= 0
        ? "It is past the point where this frame is worth painting, so the tower will drop it rather than show hours-old information."
        : `If it has not been delivered within about ${Math.max(
            1,
            Math.round(hoursLeft),
          )} hour${Math.round(hoursLeft) === 1 ? "" : "s"}, the tower drops it rather than paint a panel with information that old.`,
    );
  }

  return parts.join(" ");
}
