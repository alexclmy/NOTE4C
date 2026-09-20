import { z } from "zod";
import { DevicePowerSchema, PowerIntentSchema } from "@/core/power";
import { defaultDeviceAddress } from "@/server/config";
import { readDocument, writeDocument, type DocumentSpec } from "./atomicFile";
import { ensureDataRoot, paths } from "./paths";

/**
 * The address a fresh tower starts with: none, unless NOTE4C_DEVICE_ADDRESS
 * says otherwise.
 *
 * It used to be one real panel's LAN address written into the source, which
 * meant every clone of this repository pointed at a stranger's network out of
 * the box. Empty is the honest default: the tower is on the mock, and the
 * onboarding asks for an address before anything real can happen.
 */
export function defaultDeviceAddressValue(): string {
  return defaultDeviceAddress();
}

export const TowerStateSchema = z.object({
  schema_version: z.literal(3),
  /** The dashboard the next push would send. A pointer, never a promise. */
  selectedDashboardId: z.string().nullable(),
  /**
   * "mock" talks to the in-repo mock device and labels every device-derived
   * element SIMULATED. "real" is off by default and every real action is
   * separately confirmed.
   */
  deviceMode: z.enum(["mock", "real"]),
  deviceAddress: z.string(),
  /** Loopback address of the mock, set when dev or a test starts one. */
  mockDeviceOrigin: z.string().nullable(),
  firstRunCompletedAt: z.string().nullable(),
  density: z.enum(["comfortable", "compact"]),
  lastAutoRefreshAt: z.string().nullable(),
  lastAutoRefreshDashboardId: z.string().nullable(),
  lastAutoRefreshOutcome: z.string().max(80).nullable(),
  /**
   * A power mode the user asked for that the device may not have heard yet.
   *
   * It lives here, in the tower's durable state, precisely because a sleeping
   * device cannot be reached: the request outlives the click, the page and the
   * process, and is delivered the next time the device is actually awake. See
   * src/core/power.ts for why there is no remote wake to use instead.
   */
  powerIntent: PowerIntentSchema.nullable(),
  /**
   * When the device was last successfully read. The basis for the "next
   * expected wake" estimate, which is labelled as an estimate wherever it is
   * shown, because it is wrong whenever the device backed off after a failure.
   */
  deviceLastSeenAt: z.string().nullable(),
  /**
   * What the device said the last time it answered, and when.
   *
   * `deviceLastSeenAt` above records *that* it answered; this records *what it
   * said*, which is the only thing a failed read can honestly be described
   * against. See src/server/device/lastConfirmed.ts.
   *
   * Three properties, and each is here to stop this field from ever being the
   * reason a tower cannot read its own state:
   *
   *  - **Optional**, and `schema_version` deliberately stays at 3. Bumping it
   *    would make a rollback fatal — an older build refuses a document from the
   *    future, which is exactly the `DocumentCorruptError` this install's error
   *    log is full of. An unknown key is stripped by zod, so the previous
   *    release reads and writes this same file without noticing.
   *  - **Caught**, not validated. It embeds a *device*'s schema in the tower's
   *    own document, and the device is the one part of this system that can
   *    change without this repository being rebuilt. A power block this build
   *    cannot parse degrades to "no reading recorded", which the interface
   *    already renders honestly as `uncertain`; it must never take the selected
   *    dashboard, the device address and the pending intent down with it.
   *  - **Never load-bearing.** Nothing in the tower needs it to function. It
   *    buys one sentence: what the panel last said, and when.
   */
  lastDeviceReading: z
    .object({
      at: z.string(),
      power: DevicePowerSchema.nullable().catch(null),
      powerSupported: z.boolean().nullable().catch(null),
    })
    .nullish()
    .transform((value) => value ?? null)
    .catch(null),
});
export type TowerState = z.infer<typeof TowerStateSchema>;

export const STATE_SPEC: DocumentSpec<TowerState> = {
  schema: TowerStateSchema,
  version: 3,
  migrate: (raw, fromVersion) => {
    if (typeof raw !== "object" || raw === null) return raw;
    let next = raw as Record<string, unknown>;
    if (fromVersion <= 1) {
      next = {
        ...next,
        lastAutoRefreshAt: null,
        lastAutoRefreshDashboardId: null,
        lastAutoRefreshOutcome: null,
      };
    }
    if (fromVersion <= 2) {
      // A tower upgrading into the hybrid feature has no intent recorded and
      // has never seen the device under the new contract. Both start null
      // rather than being back-filled with a guess: an invented last-seen time
      // would produce a confident and wrong next-wake estimate on first load.
      next = { ...next, powerIntent: null, deviceLastSeenAt: null };
    }
    return { ...next, schema_version: 3 };
  },
};

export function defaultState(): TowerState {
  return {
    schema_version: 3,
    selectedDashboardId: null,
    deviceMode: "mock",
    deviceAddress: defaultDeviceAddressValue(),
    mockDeviceOrigin: null,
    firstRunCompletedAt: null,
    density: "comfortable",
    lastAutoRefreshAt: null,
    lastAutoRefreshDashboardId: null,
    lastAutoRefreshOutcome: null,
    powerIntent: null,
    deviceLastSeenAt: null,
    lastDeviceReading: null,
  };
}

export function readState(): TowerState {
  ensureDataRoot();
  return readDocument(paths.state(), STATE_SPEC) ?? defaultState();
}

export function writeState(state: TowerState): TowerState {
  ensureDataRoot();
  return writeDocument(paths.state(), STATE_SPEC, state);
}

export function updateState(patch: Partial<TowerState>): TowerState {
  return writeState({ ...readState(), ...patch });
}
