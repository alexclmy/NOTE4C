import { DeviceAddressError } from "./address";
import {
  DeviceError,
  DeviceRevisionConflict,
  DeviceUncertainError,
} from "./client";

/**
 * Which of several completely different things a failed device call was.
 *
 * Every caller in this server used to reduce a failed `client.status()` to one
 * bit — `reachable: false` — and three of them threw even that away with a
 * bare `catch {}`. The justification was sound and is still in those files: a
 * device in automatic power saving is asleep fifty-nine minutes out of sixty,
 * so "no answer" is this product's normal state and not an event worth
 * recording.
 *
 * What that reasoning missed is that "no answer" and "the tower never asked"
 * are not the same fact, and the second one is a fault. A production outage
 * ran for days with the panel answering in 70 ms while Overview said sleeping
 * and the scheduler queued frames, because the one bit could not tell a
 * sleeping device from the tower's own transport failing on the way out.
 *
 * So this is the vocabulary for that question. It does not change what any
 * caller *does* — a device that is away is still not an event, and nothing
 * here writes to disk — it changes what a caller can say.
 */
export type DeviceFailureKind =
  /**
   * Nothing answered. A sleeping panel, a powered-off one, one that is not on
   * this network and a link that dropped before the first byte came back are
   * genuinely indistinguishable from here, and the copy says so rather than
   * picking one. This is the quiet case, and it stays quiet.
   */
  | "absent"
  /**
   * Either the device was demonstrably there — it sent a status line and then
   * stopped — or the tower's own transport failed before the wire was involved
   * at all. Both are faults rather than schedules, and both were invisible.
   */
  | "transport"
  /** The device answered and said no. It is awake and it has an opinion. */
  | "refused"
  /** The device answered, and the answer did not match the contract. */
  | "contract"
  /** The deadline ran out. The tower does not know what happened. */
  | "uncertain"
  /**
   * The tower refused to make the call: no token, an address it will not
   * dial, a frame that is the wrong length. No socket was opened and no retry
   * changes any of it.
   */
  | "not_configured"
  /** Something that is not a device error at all. */
  | "unknown";

export interface DeviceFailure {
  kind: DeviceFailureKind;
  /** The DeviceError code, or a stand-in for the ones that have no code. */
  code: string;
  /** One sentence, safe for the interface and for the audit log. No secrets. */
  detail: string;
  /**
   * Whether the device proved it was there. `null` is a real value: a deadline
   * that expires while connecting proves nothing either way.
   */
  deviceAnswered: boolean | null;
  /** The OS error code when there was one, for a reader who wants it. */
  errno: string | null;
  /**
   * True when this machine answered out of its own routing table and no packet
   * was sent. Nothing may be concluded about the panel from such a failure —
   * not that it is away, and certainly not that it is asleep.
   */
  heldLocally: boolean;
  /**
   * True when this is worth telling somebody about. A device that is away is
   * not; everything else is. This is the flag a caller should branch on rather
   * than re-deriving the set of kinds.
   */
  notable: boolean;
}

/** The contract-shape codes. The device answered; the answer was wrong. */
const CONTRACT_CODES = new Set([
  "bad_status_shape",
  "bad_put_shape",
  "bad_config_shape",
  "bad_patch_shape",
]);

/**
 * Codes the tower raises before a socket exists, because it will not make the
 * call: an unpaired device, and a frame that is not frame-shaped.
 */
const NOT_CONFIGURED_CODES = new Set(["no_token", "bad_length"]);

export function describeDeviceFailure(error: unknown): DeviceFailure {
  if (error instanceof DeviceUncertainError) {
    return {
      kind: "uncertain",
      code: error.code,
      detail: error.message,
      deviceAnswered: null,
      errno: null,
      heldLocally: false,
      notable: true,
    };
  }

  if (error instanceof DeviceRevisionConflict) {
    return {
      kind: "refused",
      code: error.code,
      detail: error.message,
      deviceAnswered: true,
      errno: null,
      heldLocally: false,
      notable: true,
    };
  }

  if (error instanceof DeviceAddressError) {
    return {
      kind: "not_configured",
      code: "bad_address",
      detail: error.message,
      deviceAnswered: false,
      errno: null,
      heldLocally: false,
      notable: true,
    };
  }

  if (error instanceof DeviceError) {
    const transport = error.transport;

    if (transport !== null) {
      // The whole point of the file, and the line to read carefully.
      //
      // Silence is the sleeping-device case and stays quiet — including a
      // reset that arrives before any answer, because that is also how a
      // device dropping off Wi-Fi and the in-repo mock's simulated sleep both
      // look. Notability is bought by evidence: the device sent a status line
      // and then failed, or this process failed without touching the wire.
      // The second of those is the one the outage lived in, and it must never
      // be reported as a panel that is away.
      //
      // A call this machine refused from its own routing table is the third
      // case, and it is not silence: nothing was asked, so there is no silence
      // to interpret. It reads as `transport` — "the tower never managed to
      // ask" — rather than as `absent`, because classifying it with the
      // sleeping panel is precisely how a twenty-second kernel hold-down came
      // to be displayed as a sleeping device.
      const absent =
        transport.fault === "network" &&
        !transport.answered &&
        !transport.heldLocally;
      return {
        kind: absent ? "absent" : "transport",
        code: error.code,
        detail: error.message,
        deviceAnswered: transport.answered,
        errno: transport.errno,
        heldLocally: transport.heldLocally,
        notable: !absent,
      };
    }

    if (NOT_CONFIGURED_CODES.has(error.code)) {
      return {
        kind: "not_configured",
        code: error.code,
        detail: error.message,
        deviceAnswered: false,
        errno: null,
        heldLocally: false,
        notable: true,
      };
    }

    if (CONTRACT_CODES.has(error.code)) {
      return {
        kind: "contract",
        code: error.code,
        detail: error.message,
        deviceAnswered: true,
        errno: null,
        heldLocally: false,
        notable: true,
      };
    }

    // A DeviceError with a status came off the device's own answer. One
    // without a status and without transport diagnostics is a code path that
    // predates them; "refused" is still the closer of the two, because
    // something decided to say no.
    return {
      kind: "refused",
      code: error.code,
      detail: error.message,
      deviceAnswered: error.status !== null,
      errno: null,
      heldLocally: false,
      notable: true,
    };
  }

  return {
    kind: "unknown",
    code: "unknown",
    detail:
      error instanceof Error ? error.message : "The device call failed for an unknown reason",
    deviceAnswered: null,
    errno: null,
    heldLocally: false,
    notable: true,
  };
}
