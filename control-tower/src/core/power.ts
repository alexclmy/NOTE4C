/**
 * The hybrid low-power model, tower side.
 *
 * This file is portable: no Node, no fetch, no filesystem. Everything here is
 * a pure function of its arguments, so the vitest suite drives the awkward
 * states directly instead of trying to reproduce them against a device that
 * is, by design, usually asleep.
 *
 * The one thing this module exists to keep honest
 * ----------------------------------------------
 * **A sleeping device cannot be woken over Wi-Fi.** Deep sleep powers the
 * radio down; there is no socket listening and no packet that can change that.
 * Any UI that offers "wake the device" as a button is lying, and any client
 * that retries a request at a sleeping device is burning battery on the
 * router, not on the device.
 *
 * So the tower does the only honest thing available: it holds the request as
 * an *intent*, tells the user plainly that it is waiting, shows when the
 * device is next expected to call in, and points out that the button on the
 * device is the only immediate wake there is. `planIntent` below is the whole
 * of that logic, and it is what the server route and the scheduler both use.
 *
 * Mirrors the firmware contract in main/common/power_policy.h. The wire names
 * and bounds are duplicated here on purpose: this is a separate process that
 * has to validate before it sends, and the tests on both sides assert the same
 * literals so a drift shows up as a failure rather than as a 400 in the field.
 */

import { z } from "zod";

// ------------------------------------------------------------------ modes --

export const POWER_MODES = ["auto_saver", "interactive", "always_on"] as const;
export type PowerMode = (typeof POWER_MODES)[number];

export function isPowerMode(value: unknown): value is PowerMode {
  return (
    typeof value === "string" && (POWER_MODES as readonly string[]).includes(value)
  );
}

/** The four windows the tower offers. A closed list, like the firmware's. */
export const INTERACTIVE_MINUTES = [5, 15, 30, 60] as const;
export type InteractiveMinutes = (typeof INTERACTIVE_MINUTES)[number];

export function isInteractiveMinutes(value: unknown): value is InteractiveMinutes {
  return (
    typeof value === "number" &&
    (INTERACTIVE_MINUTES as readonly number[]).includes(value)
  );
}

export const MIN_WAKE_INTERVAL_MIN = 15;
export const MAX_WAKE_INTERVAL_MIN = 1440;
export const DEFAULT_WAKE_INTERVAL_MIN = 60;

export function isWakeIntervalMinutes(value: unknown): value is number {
  return (
    typeof value === "number" &&
    Number.isInteger(value) &&
    value >= MIN_WAKE_INTERVAL_MIN &&
    value <= MAX_WAKE_INTERVAL_MIN
  );
}

/**
 * The wake intervals the interface offers as one-press choices.
 *
 * Five, spanning fifteen minutes to four hours, and every one of them is inside
 * the firmware's own 15..1440 bound — which is checked again by the route and
 * again by the device, so this list is a convenience rather than the
 * validation. Twelve hours is deliberately not one of them: it is what "Deep
 * saver" means, and offering it twice would let a mode card and a chip
 * contradict each other about the same number.
 */
export const WAKE_INTERVAL_PRESETS = [15, 30, 60, 120, 240] as const;

/** Twice a day. What "Deep saver" asks the firmware for. */
export const DEEP_SAVER_WAKE_MIN = 720;

/**
 * Which of the three energy choices the device is actually in, or null.
 *
 * Three cards, two firmware modes and a number: `always_on` is one mode, and
 * "Balanced" and "Deep saver" are both `auto_saver` at different wake
 * intervals. That is the truth about the hardware — sleeping more and sleeping
 * less is one mode with a number — and this is the only place the two
 * vocabularies are translated.
 *
 * Read from the *effective* mode and the *effective* interval, never from what
 * was last asked for. A request the device has not picked up yet is shown as
 * pending in its own banner; selecting its card as well would be the interface
 * agreeing with itself rather than with the hardware. An open interactive
 * window selects nothing at all, because it is temporary by construction and
 * highlighting a card would say the device had been *set* to something it is
 * about to stop being.
 *
 * Null also covers "not read yet", which is not the same as Balanced: the
 * tower does not know, and a selected card would be a claim about a device it
 * has not reached.
 */
export function energyChoice(
  power: DevicePower | null,
): "ready" | "balanced" | "saver" | null {
  if (power === null) return null;
  if (power.mode === "always_on") return "ready";
  if (power.mode === "interactive") return null;
  return power.wake_interval_min >= DEEP_SAVER_WAKE_MIN ? "saver" : "balanced";
}

/** The capability string a build with the hybrid contract advertises. */
export const POWER_CAPABILITY = "power.hybrid.v1";

export function deviceSupportsPower(capabilities: readonly string[] | undefined): boolean {
  // Absent capability list means an api 1 device, which has no power contract
  // at all. Assuming support and finding out from a 400 would mean writing to
  // a device that cannot honour it.
  return Array.isArray(capabilities) && capabilities.includes(POWER_CAPABILITY);
}

/**
 * How much a mode costs, in words the UI puts next to the button.
 *
 * `always_on` carries a warning because it is the one choice that silently
 * changes battery life by two orders of magnitude, and a user who picks it
 * from a list of three equal-looking options has not been told that.
 */
export const MODE_COPY: Record<
  PowerMode,
  { label: string; detail: string; warning: string | null }
> = {
  auto_saver: {
    label: "Automatic power saving",
    detail:
      "The device sleeps between refreshes and wakes on its own schedule to fetch an update. This is the mode the battery life is quoted for.",
    warning: null,
  },
  interactive: {
    label: "Interactive, temporarily",
    detail:
      "The device stays awake and answers the API until the window runs out, then goes back to power saving on its own.",
    warning: null,
  },
  always_on: {
    label: "Always awake",
    detail: "The device never sleeps and the API is always reachable.",
    warning:
      "Always awake keeps the Wi-Fi radio on continuously. Expect battery life measured in hours rather than weeks, and leave the device on USB power if you choose it.",
  },
};

// ------------------------------------------------------- device power block --

/**
 * The `power` object on GET /api/v1/dashboard/status.
 *
 * Every numeric field that the device might not honestly know is nullable, and
 * the schema keeps it that way: `mv` and `percent` are null unless the ADC
 * calibration exists and the voltage is inside a single-cell range, and
 * `next_wake_epoch` is null unless the device's clock has actually been set.
 * A schema that defaulted these to 0 would turn "we do not know" into "zero",
 * which reads as a flat battery and a wake in 1970.
 */
export const DevicePowerSchema = z.object({
  contract: z.number().int().optional(),
  mode: z.enum(POWER_MODES),
  desired_mode: z.enum(POWER_MODES),
  ack: z.enum(["acknowledged", "pending_wake"]),
  awake: z.boolean(),
  sleep_intent: z.boolean(),
  interactive_remaining_s: z.number().int().nonnegative(),
  wake_interval_min: z.number().int(),
  timer_armed: z.boolean(),
  next_wake_in_s: z.number().int().nonnegative(),
  next_wake_epoch: z.number().int().nullable(),
  last_wake_reason: z.string(),
  /**
   * Null until a wake cycle has actually finished.
   *
   * A freshly booted device has not completed one, and the firmware renders
   * null rather than naming an outcome — because defaulting it to "updated"
   * told the tower the last update succeeded on a device that had never
   * updated. Nullable here so the tower carries that distinction instead of
   * flattening it back out with a `?? "updated"`.
   */
  last_outcome: z.string().nullable().default(null),
  /**
   * Which phase the wake budget ran out in, or null when it did not. Optional
   * because a `power.hybrid.v1` build from before this field existed omits it,
   * and a missing field is the same information as a null one.
   */
  budget_exhausted_phase: z.string().nullish().transform((v) => v ?? null),
  consecutive_failures: z.number().int().nonnegative(),
  battery: z.object({
    present: z.boolean(),
    calibrated: z.boolean(),
    plausible: z.boolean(),
    mv: z.number().int().nullable(),
    percent: z.number().int().min(0).max(100).nullable(),
  }),
  charge: z.object({
    state: z.string(),
    charging: z.boolean(),
  }),
});
export type DevicePower = z.infer<typeof DevicePowerSchema>;

/**
 * Why the battery is not being shown, in one sentence, or null when it is.
 *
 * The UI needs this because "no battery reading" with no explanation looks
 * like a bug in the tower, and the actual reasons are all things the user may
 * be able to do something about.
 */
export function batteryUnavailableReason(power: DevicePower): string | null {
  const b = power.battery;
  if (b.mv !== null && b.percent !== null) return null;
  if (!b.present) {
    return "The device did not return a battery reading.";
  }
  if (!b.calibrated) {
    return "This chip has no factory ADC calibration, so the device can measure counts but not volts. A percentage derived from uncalibrated counts would be a made-up number, so neither is shown.";
  }
  if (!b.plausible) {
    return "The measured voltage is outside the range a single-cell battery can be in, so it is measuring something other than the cell. No percentage is shown.";
  }
  return "The device did not report a usable battery reading.";
}

// ----------------------------------------------------------------- intent --

/**
 * A mode change the tower wants and may not have been able to deliver yet.
 *
 * This exists because of the one hard fact at the top of this file: a sleeping
 * device cannot be reached. Rather than fail the user's click, or pretend it
 * worked, the tower writes down what was asked for and applies it the next
 * time the device is actually there.
 */
export const PowerIntentSchema = z.object({
  mode: z.enum(POWER_MODES),
  /** Only meaningful for `interactive`. */
  interactiveMinutes: z.number().int().nullable(),
  /** Only sent when the user changed it. null leaves the device's value alone. */
  wakeIntervalMinutes: z.number().int().nullable(),
  requestedAt: z.string(),
  /** ISO timestamp of delivery, or null while it is still waiting. */
  appliedAt: z.string().nullable(),
  /** Last time the tower tried and the device was not there. */
  lastAttemptAt: z.string().nullable(),
  lastAttemptError: z.string().max(200).nullable(),
  attempts: z.number().int().nonnegative(),
});
export type PowerIntent = z.infer<typeof PowerIntentSchema>;

/**
 * How long an `interactive` intent stays worth applying.
 *
 * Six hours, and the reasoning is that "make it interactive" is a request
 * about *now*: somebody wants to change a setting in the next few minutes. A
 * device that has been unreachable for a day is not going to satisfy that, and
 * waking up into an interactive window six hours after the person gave up and
 * went to bed would drain the battery for nobody.
 */
export const INTERACTIVE_INTENT_MAX_AGE_MS = 6 * 60 * 60 * 1000;

/**
 * How long the other two modes stay worth applying. Seven days.
 *
 * These used to have no expiry at all, on the reasoning that `auto_saver` and
 * `always_on` are durable preferences rather than requests about now, so "put
 * it in power saving" is still true tomorrow. That is true of the *preference*
 * and false of the *intent*, and the difference is what makes the old
 * behaviour dishonest.
 *
 * An intent is not a preference. It is one line in the tower's list of things
 * it still owes the device, it is shown in the interface as PENDING, and the
 * only way it ever leaves the list is by being delivered. A device that is
 * reflashed, replaced, given away or simply never turned on again leaves that
 * line sitting there forever — a badge on every page saying something is about
 * to happen, about a thing that is never going to happen. Seven days is long
 * enough to cover a holiday and short enough that the badge means something.
 *
 * The preference is not lost when the intent expires: the user is told, in
 * words, that it was dropped and why, and asking again is one click.
 */
export const DURABLE_INTENT_MAX_AGE_MS = 7 * 24 * 60 * 60 * 1000;

/**
 * How many *write attempts* an intent gets before the tower stops trying.
 *
 * Attempts, not passes. `attempts` only counts occasions where a request was
 * actually put on the wire and the device refused it or the write failed;
 * waiting for a sleeping device is free and is not counted. Eight of those,
 * spread over the backoff below, is about ninety minutes of a device that is
 * awake and saying no. At that point the answer is not going to change by
 * being asked a ninth time, and the honest move is to stop and say so.
 */
export const MAX_INTENT_ATTEMPTS = 8;

/**
 * The first retry delay, and the ceiling.
 *
 * Thirty seconds is one scheduler tick: the soonest a retry could happen
 * anyway, so the first backoff costs nothing. The ceiling is thirty minutes,
 * which is about the shortest realistic wake interval — past that the tower
 * would be retrying more often than the device is even awake.
 */
export const INTENT_RETRY_BASE_MS = 30_000;
export const INTENT_RETRY_MAX_MS = 30 * 60_000;

/**
 * How long to wait after `attempts` failed writes. Bounded exponential.
 *
 * Before this there was no backoff of any kind: a reachable device that
 * refused the write left the intent in place, and the scheduler re-sent it
 * every thirty seconds for as long as the device kept refusing — a hundred and
 * twenty writes an hour at a panel that had already said no, each one a socket
 * on a device that serves four of them and each one an audit line. Doubling,
 * capped, turns that into eight lines over ninety minutes.
 */
export function intentRetryDelayMs(attempts: number): number {
  if (attempts <= 0) return 0;
  const exponent = Math.min(attempts - 1, 30);
  return Math.min(INTENT_RETRY_BASE_MS * 2 ** exponent, INTENT_RETRY_MAX_MS);
}

/**
 * When a failed intent may next be put on the wire, or null if it may now.
 *
 * Null covers both "never attempted" and "the last attempt was long enough
 * ago"; the caller compares against its own `now` rather than this function
 * reading a clock.
 */
export function intentRetryAt(intent: PowerIntent): Date | null {
  if (intent.attempts <= 0 || intent.lastAttemptAt === null) return null;
  const last = new Date(intent.lastAttemptAt).getTime();
  if (!Number.isFinite(last)) return null;
  return new Date(last + intentRetryDelayMs(intent.attempts));
}

/** How long an intent of this mode is worth applying for. */
export function intentMaxAgeMs(mode: PowerMode): number {
  return mode === "interactive"
    ? INTERACTIVE_INTENT_MAX_AGE_MS
    : DURABLE_INTENT_MAX_AGE_MS;
}

/**
 * Why this intent should be given up on, or null to keep it.
 *
 * Two ways out, and they are different failures. Age means nobody was ever
 * there to receive it; attempts means somebody was there and kept refusing.
 * The sentences say which, because the two call for different things from the
 * person reading them.
 */
export function intentExpiryReason(
  intent: PowerIntent,
  now: Date,
): string | null {
  if (intent.appliedAt !== null) return null;

  if (intent.attempts >= MAX_INTENT_ATTEMPTS) {
    return `The device was reachable and refused this change ${intent.attempts} times, so the tower stopped retrying rather than keep writing to a device that has said no. ${
      intent.lastAttemptError ?? "No reason was reported."
    }`;
  }

  const maxAge = intentMaxAgeMs(intent.mode);
  const age = now.getTime() - new Date(intent.requestedAt).getTime();
  if (!Number.isFinite(age) || age <= maxAge) return null;

  if (intent.mode === "interactive") {
    return "The device was not reachable within six hours, so this temporary interactive request was dropped rather than applied to a device nobody is standing at any more.";
  }
  const days = Math.round(maxAge / (24 * 60 * 60 * 1000));
  return `This request has been waiting ${days} days without the device ever being reachable to receive it, so the tower dropped it rather than keep showing a pending change that was never going to arrive. Ask again if it is still what you want.`;
}

/** Kept for callers that only need the boolean. */
export function intentIsStale(intent: PowerIntent, now: Date): boolean {
  return intentExpiryReason(intent, now) !== null;
}

/** The config fields a given intent would write. Mirrors the firmware table. */
export function intentToPatch(intent: PowerIntent): Record<string, string | number> {
  const set: Record<string, string | number> = { "power.mode": intent.mode };
  if (intent.mode === "interactive" && intent.interactiveMinutes !== null) {
    set["power.interactive_min"] = intent.interactiveMinutes;
  }
  if (intent.wakeIntervalMinutes !== null) {
    set["power.wake_interval_min"] = intent.wakeIntervalMinutes;
  }
  return set;
}

/**
 * Is the device already in the state this intent asks for?
 *
 * Compared against the *effective* mode, not the desired one, because the
 * question the tower is answering is "does anything still need writing", and
 * a device that merely intends to be interactive later is not interactive now.
 *
 * An interactive intent is never considered satisfied by an open window: the
 * user asking for "30 minutes" wants thirty minutes from now, not whatever is
 * left of a window that opened twenty minutes ago.
 */
export function intentIsSatisfied(intent: PowerIntent, power: DevicePower): boolean {
  if (intent.mode === "interactive") return false;
  if (power.mode !== intent.mode) return false;
  if (
    intent.wakeIntervalMinutes !== null &&
    power.wake_interval_min !== intent.wakeIntervalMinutes
  ) {
    return false;
  }
  return true;
}

export type IntentDisposition =
  /** Nothing recorded, or it has already been delivered. */
  | { kind: "none" }
  /** Recorded, and the device is already in that state. Clear it. */
  | { kind: "satisfied" }
  /** The device is here: write these fields now. */
  | { kind: "apply"; set: Record<string, string | number> }
  /** The device is asleep or unreachable. Keep waiting; say so. */
  | { kind: "waiting"; reason: string }
  /**
   * The device is here and the intent still needs writing, but the last write
   * failed and the backoff has not run out. Distinct from `waiting`, which is
   * about a device that is not there: this one is about not hammering one that
   * is.
   */
  | { kind: "backoff"; retryAt: string; reason: string }
  /** Give up and clear it, with a reason the user can read. */
  | { kind: "expired"; reason: string };

export interface PlanIntentInput {
  intent: PowerIntent | null;
  /** The device's own power block, or null when it could not be read. */
  power: DevicePower | null;
  reachable: boolean;
  /** False for an api 1 device, or a build without the hybrid contract. */
  supported: boolean;
  now: Date;
}

/**
 * The single decision every caller shares.
 *
 * The status route calls it to describe what is happening, and the apply path
 * calls it to decide whether to write. One function so the sentence the user
 * reads and the action the tower takes cannot disagree.
 */
export function planIntent(input: PlanIntentInput): IntentDisposition {
  const { intent, power, reachable, supported, now } = input;
  if (intent === null || intent.appliedAt !== null) return { kind: "none" };

  const expiry = intentExpiryReason(intent, now);
  if (expiry !== null) return { kind: "expired", reason: expiry };

  if (!reachable) {
    return {
      kind: "waiting",
      reason:
        "The device is not answering, which is what a sleeping device looks like. Nothing on the network can wake it: this will be applied the next time it wakes on its own, or immediately if you press the button on the device.",
    };
  }

  // Support is checked before the power block is required, and the order
  // matters. A device that answered but advertises no hybrid contract is not
  // going to grow one by being waited for, so holding the intent would mean
  // waiting forever for something that cannot happen. Only a device that both
  // answered and claims the contract, yet sent no power block, is worth
  // another look later.
  if (!supported) {
    return {
      kind: "expired",
      reason:
        "This device's firmware does not advertise the hybrid power contract, so there is nothing to apply the request to.",
    };
  }

  if (power === null) {
    return {
      kind: "waiting",
      reason:
        "The device answered but did not report a power state, so the tower has nothing to compare against. This will be retried on the next read.",
    };
  }

  if (intentIsSatisfied(intent, power)) return { kind: "satisfied" };

  // The device is here and something needs writing. One last question: did the
  // last write fail, and if so has the backoff run out? Checked here rather
  // than in the server so that the sentence the status route shows and the
  // decision the scheduler takes come from the same call, which is the whole
  // reason `planIntent` exists.
  const retryAt = intentRetryAt(intent);
  if (retryAt !== null && now.getTime() < retryAt.getTime()) {
    const seconds = Math.ceil((retryAt.getTime() - now.getTime()) / 1000);
    const waitFor =
      seconds >= 120 ? `${Math.ceil(seconds / 60)} minutes` : `${seconds} seconds`;
    return {
      kind: "backoff",
      retryAt: retryAt.toISOString(),
      reason: `The device refused this change ${intent.attempts} time${
        intent.attempts === 1 ? "" : "s"
      }, so the tower is waiting about ${waitFor} before trying again rather than re-sending it every thirty seconds. ${
        intent.lastAttemptError ?? ""
      }`.trim(),
    };
  }

  return { kind: "apply", set: intentToPatch(intent) };
}

// ------------------------------------------------------------ next wake --

export interface NextWakeEstimate {
  at: Date;
  /**
   * True when this is the tower's arithmetic rather than the device's own
   * answer. The UI must render the two differently: one is a fact the device
   * reported, the other is a guess that is wrong whenever the device took
   * longer than expected or failed and backed off.
   */
  estimated: boolean;
}

/**
 * When the device is next expected to be reachable.
 *
 * Two sources, and the difference is reported rather than smoothed over:
 *
 *  - The device armed a timer and told us the epoch. That is a fact, and it is
 *    only available when the device's clock has been set.
 *  - Otherwise the tower adds the wake interval to when it last saw the
 *    device. That is an estimate, and it is wrong whenever the device backed
 *    off after a failed cycle, which is exactly when the user is most likely
 *    to be looking at this field.
 *
 * Returns null when there is no basis for either, rather than inventing one.
 */
export function estimateNextWake(
  power: DevicePower | null,
  lastSeenAt: Date | null,
): NextWakeEstimate | null {
  if (power !== null && power.timer_armed && power.next_wake_epoch !== null) {
    return { at: new Date(power.next_wake_epoch * 1000), estimated: false };
  }
  if (lastSeenAt === null) return null;
  const interval = power?.wake_interval_min ?? DEFAULT_WAKE_INTERVAL_MIN;
  if (!isWakeIntervalMinutes(interval)) return null;
  return {
    at: new Date(lastSeenAt.getTime() + interval * 60_000),
    estimated: true,
  };
}

/**
 * The sentence the UI shows about reachability, given what was actually read.
 *
 * Deliberately never says "offline" or "disconnected": for this device, not
 * answering is the *designed* behaviour most of the time, and language that
 * reads as a fault would train the user to ignore a real one.
 */
export function describeReachability(
  reachable: boolean,
  power: DevicePower | null,
): string {
  if (!reachable) {
    return "Not answering. In automatic power saving this is the normal state between refreshes: the radio is off and no request can reach it.";
  }
  if (power === null) {
    return "Answering, but this build does not report a power state.";
  }
  if (power.mode === "always_on") {
    return "Awake, and set to stay awake.";
  }
  if (power.mode === "interactive") {
    const minutes = Math.ceil(power.interactive_remaining_s / 60);
    return `Awake for about ${minutes} more minute${minutes === 1 ? "" : "s"}, then back to power saving on its own.`;
  }
  if (power.sleep_intent) {
    return "Awake, and about to go back to sleep.";
  }
  return "Awake.";
}

// ------------------------------------------------------------ idempotency --

/**
 * How long two "sleep now" requests are treated as the same request.
 *
 * Sixty seconds. Long enough to cover a double-click, a client retry after a
 * socket timeout, and a user pressing the button again because the page had
 * not visibly updated yet — which are the three ways a single intention turns
 * into two requests. Short enough that a genuine second request a minute later
 * is honoured.
 *
 * The cost of the window being too long is nil in practice: the second request
 * would be aimed at a device that is already asleep and therefore unreachable
 * anyway, so it could not have done anything.
 */
export const SLEEP_IDEMPOTENCY_WINDOW_MS = 60_000;

/**
 * A stable idempotency key for "sleep now".
 *
 * Deterministic, so the same intention produces the same key and the device's
 * own idempotency ring collapses the duplicates. The alternative the tower had
 * was `randomUUID()` per call, which satisfies the firmware's requirement that
 * the header be *present* while defeating the thing the header is for: every
 * retry looked like a new action and scheduled another sleep.
 *
 * Derived from a coarse time bucket rather than stored, because there is no
 * per-request identity to key off — "go to sleep" carries no payload. A caller
 * that does have a stable identity (a UI that generates one per click) should
 * pass its own key instead; this is the fallback.
 *
 * @param now       the moment the request is being made.
 * @param windowMs  bucket width. Two calls in the same bucket share a key.
 */
export function sleepIdempotencyKey(
  now: Date,
  windowMs: number = SLEEP_IDEMPOTENCY_WINDOW_MS,
): string {
  const bucket = Math.floor(now.getTime() / windowMs);
  // Formatted as a UUID because that is what the firmware's idempotency ring
  // stores and what every other caller sends; a differently shaped key would
  // work but would make the wire harder to read in a capture.
  const hex = bucket.toString(16).padStart(12, "0").slice(-12);
  return `00000000-0000-4000-8000-${hex}`;
}

/**
 * What the device's button can do that the network cannot.
 *
 * One string, used everywhere the tower would otherwise be tempted to offer a
 * "wake now" control. There is no such control, and this says why.
 */
export const NO_REMOTE_WAKE_NOTE =
  "A sleeping device has its Wi-Fi radio powered down, so no command from here can wake it. The button on the device wakes it immediately; anything set here is applied the next time it wakes.";

/**
 * What the one-click "Interactive 15 min" request actually promises.
 *
 * The shell's rail and the state strip both offer that button, and both used to
 * explain it in a `title` attribute — invisible to a finger, invisible to a
 * keyboard, and therefore invisible on the form factor where the button is most
 * pressed. It is written once, here, beside the note it builds on, so the two
 * places cannot drift into promising different things about the same POST.
 */
export const INTERACTIVE_REQUEST_NOTE =
  `Asks the device to stay awake for 15 minutes. If it is awake the request is applied straight away; if it is asleep it is held until the device next wakes. ${NO_REMOTE_WAKE_NOTE}`;

// ------------------------------------------------------------ device state --

/**
 * The five words this product uses for "what is the device doing".
 *
 * Ordered by how they are decided rather than alphabetically, and closed on
 * purpose: every page renders one of these five and none of them invents a
 * sixth. Before this existed each page decided for itself, which is how
 * `asleep` — the *nominal* state of a device designed to sleep — came to be
 * drawn with the yellow PENDING badge on one page and the red UNREACHABLE one
 * on another.
 */
export const DEVICE_STATES = [
  "awake",
  "asleep",
  "pending",
  "uncertain",
  "unreachable",
] as const;
export type DeviceState = (typeof DEVICE_STATES)[number];

/**
 * How late a device may be before silence stops being ordinary.
 *
 * Fifteen minutes past the expected wake. The expectation is usually the
 * tower's own arithmetic (see `estimateNextWake`), and it is wrong whenever
 * the device backed off after a failed cycle — which is precisely when a user
 * is looking at this. A grace shorter than the shortest wake interval would
 * turn every backed-off retry into a red badge, and a device that cries wolf
 * about its own designed behaviour trains its owner to ignore it.
 */
export const OVERDUE_GRACE_MS = 15 * 60_000;

/**
 * The last reading the device itself gave, and when it gave it.
 *
 * A failed read tells the tower nothing about the panel — see the note on
 * `deriveDeviceState` — so the only thing that can say what the panel was doing
 * is the last time it actually answered. That answer is a fact with a
 * timestamp, and both halves matter: a device that said `auto_saver` two
 * minutes ago and one that said it two days ago support very different
 * sentences, and the age is shown wherever the reading is used.
 */
export interface ConfirmedReading {
  /** When the device answered. ISO 8601. */
  at: string;
  /** What it reported then, or null for a firmware with no power contract. */
  power: DevicePower | null;
  /** Whether that firmware advertised the hybrid power contract. */
  powerSupported: boolean | null;
}

export interface DeviceStateInput {
  /** Did the last read reach the device? */
  reachable: boolean;
  /**
   * Was the device actually asked?
   *
   * Defaults to true, because until this existed every caller had asked — a
   * page could not render at all until a read had come back, which is the
   * defect this field is part of correcting. A page that paints from the
   * tower's own durable record before any socket is opened passes `false`, and
   * that is not a third flavour of failure: it is the absence of an attempt.
   *
   * The distinction is the same one the rest of this file is built on. A
   * failed read proves the panel could not be reached *from here, just now*;
   * a read that never happened proves nothing whatsoever, so none of the
   * branches below that assert `unreachable` may be taken on it. Treating the
   * two alike would put a red UNREACHABLE on a page that had not yet asked
   * anything — the same class of unearned claim as reporting a woken panel
   * as asleep.
   */
  observed?: boolean;
  /** The device's own power block, or null when it could not be read. */
  power: DevicePower | null;
  /** False for an api 1 device; null when it has never been determined. */
  powerSupported: boolean | null;
  /** A mode change the tower is holding, if any. */
  intent: PowerIntent | null;
  /** The mock never sleeps, so silence from it means something different. */
  deviceMode: "mock" | "real";
  /** As the status route renders it. */
  nextWake: { at: string; estimated: boolean } | null;
  deviceLastSeenAt: string | null;
  /**
   * The last answer the device gave, whenever that was.
   *
   * Optional so that a caller which only has a last-known power block can keep
   * passing it as `power`; the two are folded together below. Null means the
   * tower has never recorded an answer from this device, which is its own
   * state — `uncertain` — and never a reason to guess at `asleep`.
   */
  lastConfirmed?: ConfirmedReading | null;
  now?: Date;
}

export interface DeviceStateReading {
  state: DeviceState;
  /** The one word the badge shows. Same word on every page. */
  label: string;
  /** Why, in a sentence a user can act on. Never "offline". */
  reason: string;
  /**
   * True when this state is the product working as designed. The UI uses it to
   * decide whether the reading deserves any visual weight at all: a sleeping
   * device is not news.
   */
  nominal: boolean;
}

const STATE_LABEL: Record<DeviceState, string> = {
  awake: "Awake",
  asleep: "Asleep",
  pending: "Pending",
  uncertain: "Uncertain",
  unreachable: "Unreachable",
};

function reading(
  state: DeviceState,
  reason: string,
  nominal = false,
): DeviceStateReading {
  return { state, label: STATE_LABEL[state], reason, nominal };
}

/**
 * The three questions a person actually asks, answered per state.
 *
 * `deriveDeviceState` already returns the sentence for "what do we know" — it
 * is `reading.reason`, and it is the one the badge carries — so this covers the
 * other two: what can be done right now, and what happens next if nothing is.
 * They live here rather than in the page because the Overview and the Device
 * page both render them, and because a state word whose consequences are
 * written in two components is a state word with two meanings.
 *
 * Neither table ever offers an action that does not exist. In particular
 * nothing here says "wake it": see NO_REMOTE_WAKE_NOTE.
 */
export const DEVICE_ACTION_COPY: Record<DeviceState, string> = {
  awake:
    "Send a composition now — the panel will refresh in about 25 seconds.",
  asleep:
    "Prepare and queue anything you like. It will be applied at the next wake. Wi-Fi cannot wake it — only the button on the device can.",
  pending:
    "Nothing to do. The change the tower is holding goes out the moment the device is there to take it.",
  uncertain:
    "Re-read the device, or carry on and let the next wake settle it. Both are recorded.",
  unreachable:
    "Check its power and Wi-Fi. The panel still shows its last image — nothing is lost.",
};

/**
 * What happens next, with the wake time filled in when there is one.
 *
 * A function rather than a table because two of the five sentences are only
 * true if the tower actually has a next wake to name, and a template with an
 * empty slot in it — "Next wake around ." — is worse than the sentence that
 * does not mention one.
 */
export function describeWhatHappensNext(
  state: DeviceState,
  nextWakeLabel: string | null,
): string {
  switch (state) {
    case "awake":
      return "When nothing has happened for a while it goes back to sleep on its own, to save battery.";
    case "asleep":
      return nextWakeLabel === null
        ? "It will wake on its own schedule. Queued changes apply then, automatically."
        : `Next wake around ${nextWakeLabel}. Queued changes apply then, automatically.`;
    case "pending":
      return nextWakeLabel === null
        ? "The tower applies the change the next time the device is reachable."
        : `The tower applies the change at the next wake, around ${nextWakeLabel}.`;
    case "uncertain":
      return "The tower keeps listening. A wake that actually happens settles this on its own; Advanced has the full contact log.";
    default:
      return "The tower keeps listening and will say so the moment the device calls in. Advanced has the full contact log.";
  }
}

/**
 * How long ago, in words, for a sentence rather than a table.
 *
 * `relativeTime` in src/ui/api.ts does this for the interface, but it is a
 * client module and these sentences are built on the server too. Coarse on
 * purpose: the age of a reading is context for a judgement, not a measurement.
 */
export function agoLabel(from: Date, now: Date): string {
  const seconds = Math.round((now.getTime() - from.getTime()) / 1000);
  if (seconds < 0) return "in the future";
  if (seconds < 60) return "less than a minute ago";
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes} minute${minutes === 1 ? "" : "s"} ago`;
  const hours = Math.round(minutes / 60);
  if (hours < 48) return `${hours} hour${hours === 1 ? "" : "s"} ago`;
  return `${Math.round(hours / 24)} days ago`;
}

/** A clock time for a sentence, in the reader's own locale where there is one. */
function clockLabel(at: Date): string {
  return at.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
}

/**
 * The device's own last word, whatever shape the caller had it in.
 *
 * Callers that carry a full `ConfirmedReading` hand one in. Callers that only
 * ever had a last-known power block — which is how this function was fed
 * before the reading was recorded — pass it as `power` alongside
 * `deviceLastSeenAt`, and the two mean the same thing.
 */
function evidenceFrom(input: DeviceStateInput): ConfirmedReading | null {
  const confirmed = input.lastConfirmed ?? null;
  if (confirmed !== null && !Number.isNaN(new Date(confirmed.at).getTime())) {
    return confirmed;
  }
  if (input.power !== null && input.deviceLastSeenAt !== null) {
    return {
      at: input.deviceLastSeenAt,
      power: input.power,
      powerSupported: input.powerSupported,
    };
  }
  return null;
}

/**
 * Did the device itself account for not answering?
 *
 * Three things count, and all three are statements the *device* made about
 * itself the last time it spoke:
 *
 *  - it was running the automatic sleep/wake cycle (`auto_saver`), whose whole
 *    contract is that the radio is off between wakes;
 *  - it declared it was about to sleep (`sleep_intent`);
 *  - it armed its own wake timer, which it only does on the way down.
 *
 * What does *not* count is the tower noticing that an interactive window has
 * since run out. The device does return to power saving when one expires — but
 * the tower did not see it do so, and "it probably went to sleep" is an
 * inference, not a reading. That inference is exactly what reported a panel
 * woken thirty seconds earlier with the BOOT button as `Asleep`.
 */
function deviceAccountedForSilence(power: DevicePower | null): boolean {
  if (power === null) return false;
  if (power.sleep_intent) return true;
  if (power.timer_armed) return true;
  return power.mode === "auto_saver";
}

/**
 * What can be said before the tower has asked anything at all.
 *
 * Three words are reachable from here and two of them are not. `awake` is not,
 * because the only proof of awake is an answer. `unreachable` is not, because
 * nothing has been sent: the page is painting from the tower's own record
 * while the read it triggered is still in flight, and a claim that the panel
 * cannot be reached would be a fact the tower has not established. That leaves:
 *
 *  - `pending`, when the tower is holding a mode change. That is the tower's
 *    own fact about its own queue and needs no device to confirm it.
 *  - `asleep`, and only on the device's own account of itself — the same
 *    `deviceAccountedForSilence` evidence the answered path uses. A panel in
 *    automatic power saving is asleep whether or not anyone has just knocked,
 *    and saying so is what keeps a sleeping device an ordinary state rather
 *    than a page that has to finish a network round trip to look normal.
 *  - `uncertain` otherwise, which is the honest word for "not asked yet".
 *
 * Every sentence says so, because a reader who sees `Uncertain` deserves to
 * know whether the tower tried and failed or has not tried yet.
 */
function notYetAsked(input: DeviceStateInput, now: Date): DeviceStateReading {
  const evidence = evidenceFrom(input);
  const evidencePower = evidence?.power ?? null;
  const evidenceAt = evidence ? new Date(evidence.at) : null;
  const stamp =
    evidenceAt === null
      ? ""
      : ` It last answered at ${clockLabel(evidenceAt)}, ${agoLabel(evidenceAt, now)}.`;

  const intent = input.intent;
  if (intent !== null && intent.appliedAt === null) {
    return reading(
      "pending",
      `${MODE_COPY[intent.mode].label} is waiting to be applied. ${NO_REMOTE_WAKE_NOTE}`,
    );
  }

  if (deviceAccountedForSilence(evidencePower)) {
    return reading(
      "asleep",
      `The last thing the device said accounts for its silence: ${
        evidencePower?.sleep_intent
          ? "it reported it was about to sleep"
          : evidencePower?.timer_armed
            ? "it had armed its own wake timer"
            : "it was running automatic power saving, which sleeps between refreshes"
      }.${stamp} The tower is reading it again now.`,
      true,
    );
  }

  return reading(
    "uncertain",
    evidence === null
      ? "The tower has not read the device yet, and has no earlier reading to go on. This is what it knows so far; the read now in flight will settle it."
      : `The tower has not read the device yet on this page.${stamp} This is what it knew then; the read now in flight will settle it.`,
  );
}

/**
 * The one place a reading of the device becomes a word.
 *
 * THE RULE THIS FILE EXISTS FOR
 * -----------------------------
 * A read that failed says one thing and one thing only: *the tower could not
 * reach the panel just now*. It does not say the panel is asleep. Those were
 * conflated here for the whole of one incident: the status route reports
 * `power: null` whenever a read fails, this function's `always_on` and
 * `interactive` branches therefore never had a power block to look at, and
 * every failed read fell through to `asleep` — "the designed state between
 * refreshes". So a panel that had just been woken with the BOOT button, whose
 * own `/api/v1/dashboard/status` answered `awake: true, mode: interactive` in
 * 55 ms to anything else on the LAN, was reported as sleeping, next to a red
 * UNREACHABLE banner derived from the very same failed read.
 *
 * `asleep` is therefore only ever said on evidence the *device* produced:
 * either it answered and reported `awake: false`, or the last time it answered
 * it said it was running the automatic sleep/wake cycle, or it declared an
 * intention to sleep and armed a timer. Never on silence alone.
 *
 * Every interesting case is one where two words could apply, so the order is
 * the substance:
 *
 *  1. It answered. That is a fact, and it beats everything else — except the
 *     device's own admission that it has not yet taken up what it was asked
 *     for (`pending`), or its own report that it is not awake (`asleep`).
 *  2. It did not answer, and it *owes* us an answer — the mock, a firmware
 *     with no sleep contract, `always_on`, or an interactive window that had
 *     not run out when it last spoke. Silence there is a fault: `unreachable`.
 *  3. It did not answer and the tower has no idea what to expect, or it is
 *     late past the grace. Neither is a fault the tower can assert, so:
 *     `uncertain`.
 *  4. It did not answer, the device's own last word accounts for that, and
 *     something is queued for its next wake: `pending`.
 *  5. It did not answer and the device's own last word accounts for that:
 *     `asleep`, and that is the product working.
 *  6. It did not answer and nothing the device ever said accounts for it:
 *     `uncertain`. This is the case that used to be answered with a confident
 *     `asleep`.
 */
export function deriveDeviceState(input: DeviceStateInput): DeviceStateReading {
  const { reachable, power, powerSupported, intent, deviceMode, nextWake } = input;
  const now = input.now ?? new Date();

  if (reachable) {
    if (power !== null && power.ack === "pending_wake") {
      return reading(
        "pending",
        `The device is answering but reports it has not yet taken up ${MODE_COPY[
          power.desired_mode
        ].label.toLowerCase()}.`,
      );
    }
    // The device's own word about itself, and it outranks the mode: a panel in
    // automatic power saving that answers `awake: true` is awake, and one that
    // answers `awake: false` is not, whatever mode it is in. This is the only
    // positive proof of sleep there is, because it is the only one the device
    // ever states about the present moment.
    if (power !== null && !power.awake) {
      return reading(
        "asleep",
        "The device answered and reports it is not awake: it is on its way into power saving, or serving this read from a wake window it is about to end.",
        true,
      );
    }
    return reading(
      "awake",
      power?.mode === "always_on"
        ? "Answering, and set to stay awake."
        : "The device answered the last read.",
      true,
    );
  }

  // From here the device did not answer. That is a fact about the tower's last
  // attempt and about nothing else, so every word below is chosen from what the
  // device itself last said — never from the silence.

  if (input.observed === false) {
    return notYetAsked(input, now);
  }

  if (deviceMode === "mock") {
    return reading(
      "unreachable",
      "The tower is pointed at the in-repo mock, which does not sleep. Silence from it means the mock is not running, not that a device is resting.",
    );
  }

  const evidence = evidenceFrom(input);
  const evidencePower = evidence?.power ?? null;
  const evidenceAt = evidence ? new Date(evidence.at) : null;
  const evidenceSupported = evidence ? evidence.powerSupported : powerSupported;
  const stamp =
    evidenceAt === null
      ? ""
      : ` It last answered at ${clockLabel(evidenceAt)}, ${agoLabel(evidenceAt, now)}.`;

  if (evidenceSupported === false) {
    return reading(
      "unreachable",
      `This firmware advertises no hybrid power contract, so it has no sleep state to be in. Not answering means it is not on the network.${stamp}`,
    );
  }

  if (evidencePower !== null && evidencePower.mode === "always_on") {
    return reading(
      "unreachable",
      `The device was last known to be set to stay awake, so it should be answering. It is not.${stamp}`,
    );
  }

  if (
    evidencePower !== null &&
    evidencePower.mode === "interactive" &&
    evidenceAt !== null &&
    evidenceAt.getTime() + evidencePower.interactive_remaining_s * 1000 > now.getTime()
  ) {
    return reading(
      "unreachable",
      `An interactive window was still open when the tower last looked, so the device should be answering. It is not.${stamp}`,
    );
  }

  if (evidence === null && input.deviceLastSeenAt === null && nextWake === null) {
    return reading(
      "uncertain",
      "The tower has never reached this device, so it cannot say whether it is asleep as designed or simply not there.",
    );
  }

  if (nextWake !== null) {
    const due = new Date(nextWake.at).getTime();
    if (Number.isFinite(due) && now.getTime() > due + OVERDUE_GRACE_MS) {
      return reading(
        "uncertain",
        (nextWake.estimated
          ? "The device is past the wake the tower estimated for it. The estimate is wrong whenever the device backed off after a failed cycle, so this is late rather than proof of a fault."
          : "The device armed a timer for a wake that has passed, and it has not called in since.") + stamp,
      );
    }
  }

  // Unchanged in rank: a request that is waiting is the actionable fact, and
  // its sentence makes no claim about what the device is doing.
  const pending = intent !== null && intent.appliedAt === null;
  if (pending) {
    return reading(
      "pending",
      `${MODE_COPY[(intent as PowerIntent).mode].label} is waiting to be applied. ${NO_REMOTE_WAKE_NOTE}`,
    );
  }

  if (deviceAccountedForSilence(evidencePower)) {
    return reading(
      "asleep",
      `Not answering, and the last thing the device said accounts for it: ${
        evidencePower?.sleep_intent
          ? "it reported it was about to sleep"
          : evidencePower?.timer_armed
            ? "it had armed its own wake timer"
            : "it was running automatic power saving, which sleeps between refreshes"
      }. The radio is off and no request can reach it.${stamp}`,
      true,
    );
  }

  // Nothing the device ever said accounts for this silence. The tower will not
  // fill that gap with a guess: a failed read is a failed read.
  return reading(
    "uncertain",
    evidence === null
      ? `The tower could not reach the device, and has no reading from it to explain why — so it cannot tell a panel sleeping as designed from one that is not there.${
          input.deviceLastSeenAt === null
            ? ""
            : ` It last recorded contact ${agoLabel(new Date(input.deviceLastSeenAt), now)}.`
        } A failed read proves the panel is unreachable from here right now, not that it is asleep. The next answer settles it.`
      : `The tower could not reach the device, and the last thing the device said does not account for that: no sleep was announced and no wake timer was armed.${stamp} A failed read proves the panel is unreachable from here right now, not that it is asleep.`,
  );
}
