import { NO_REMOTE_WAKE_NOTE, estimateNextWake } from "@/core/power";
import { blockingPush, readPushes } from "./ledger";
import { readLastConfirmed } from "./lastConfirmed";
import { describeIntent } from "./powerIntent";
import { deviceTokenIsSet } from "./token";
import { readState } from "../store/state";

/**
 * Everything the two device-facing pages need in order to paint, with no
 * socket opened at all.
 *
 * WHY THIS EXISTS
 * ---------------
 * `/overview` and `/device` used to have no way to render anything until a
 * device read came back, and a device read is the slowest thing in this
 * product. Measured on the mock with a socket that is accepted and never
 * answered — which is what a panel behind a dropped-packet firewall, or a
 * `node` binary without macOS Local Network permission, actually looks like —
 * a cold `/device` load cost three serialised reads and a cold `/overview`
 * load cost two. At the ten-second transport timeout that is thirty seconds
 * and twenty seconds of skeleton, for pages whose content is almost entirely
 * local files.
 *
 * So the first paint comes from here. Everything in this payload is a
 * `readFileSync` of a small JSON document in the tower's own data directory,
 * plus the in-memory reading from the last time the device answered this
 * process. It costs under a millisecond and it cannot block.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * It does not claim the device was read. `observed` is false and stays false,
 * and every consumer treats that as "nobody has asked", never as a failed
 * read — see the note on `DeviceStateInput.observed`. A payload from here says
 * what the tower knows and when it learned it; the read that establishes what
 * is true *now* runs behind the page and replaces this the moment it lands.
 *
 * It also writes nothing. It is the read-only half of a route that is already
 * required to stay a read.
 */
export interface LastKnownDevice {
  /** Always false here. Nothing was asked, so nothing can be asserted. */
  observed: false;
  /**
   * Always false, and paired with `observed: false` it means "not asked".
   * Carried so a payload from here is the same shape as one from a real read
   * and the interface does not have to hold two.
   */
  reachable: false;
  power: null;
  powerSupported: null;
  powerIntent: ReturnType<typeof describeIntent>["intent"];
  powerIntentDisposition: string;
  powerIntentDetail: string;
  batteryUnavailableReason: null;
  reachabilityNote: string;
  noRemoteWake: string;
  deviceLastSeenAt: string | null;
  nextWake: { at: string; estimated: boolean } | null;
  deviceMode: "mock" | "real";
  /**
   * Read from the tower's own state rather than from a client.
   *
   * `deviceContext()` would answer this too, but in mock mode it starts the
   * in-process panel to do it, and this path's whole promise is that it starts
   * nothing. The state document already records which device the tower is
   * pointed at, which is the same fact.
   */
  simulated: boolean;
  tokenConfigured: boolean;
  deviceAddress: string | null;
  readAt: string;
  lastConfirmed: ReturnType<typeof readLastConfirmed>;
  blocking: ReturnType<typeof blockingPush>;
  lastPush: ReturnType<typeof readPushes>[number] | null;
}

export function readLastKnownDevice(now: Date = new Date()): LastKnownDevice {
  const state = readState();
  const lastConfirmed = readLastConfirmed();
  // Described, never delivered — the same `describeIntent` the status route
  // uses, run with no device, so the sentence a reader sees before the read
  // lands is the one the read will either confirm or replace.
  const described = describeIntent(null, { reachable: false });
  const nextWake = estimateNextWake(
    lastConfirmed?.power ?? null,
    state.deviceLastSeenAt ? new Date(state.deviceLastSeenAt) : null,
  );

  return {
    observed: false,
    reachable: false,
    power: null,
    powerSupported: null,
    powerIntent: described.intent,
    powerIntentDisposition: described.disposition.kind,
    powerIntentDetail: described.detail,
    batteryUnavailableReason: null,
    // Not `describeReachability(false, …)`, which opens "Not answering." —
    // a fact about an attempt, and no attempt was made. The sentence has to
    // survive the case where the panel is in fact wide awake and nobody has
    // knocked, which is precisely the case this path exists for.
    reachabilityNote:
      "Not read yet. This is what the tower last recorded; it is asking the device now.",
    noRemoteWake: NO_REMOTE_WAKE_NOTE,
    deviceLastSeenAt: state.deviceLastSeenAt,
    nextWake: nextWake
      ? { at: nextWake.at.toISOString(), estimated: nextWake.estimated }
      : null,
    deviceMode: state.deviceMode,
    simulated: state.deviceMode === "mock",
    // The mock is always paired; a real device's token is one 0600 file read.
    tokenConfigured: state.deviceMode === "mock" || deviceTokenIsSet(),
    deviceAddress: state.deviceMode === "mock" ? null : state.deviceAddress,
    readAt: now.toISOString(),
    lastConfirmed,
    blocking: blockingPush(),
    lastPush: readPushes()[0] ?? null,
  };
}
