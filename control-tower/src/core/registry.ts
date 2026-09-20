/**
 * Device capability and settings registry.
 *
 * The Device page renders entirely from this table, so honesty is structural
 * rather than editorial: an entry whose capability the device did not report
 * cannot render an editable control, because the function that decides that
 * takes the device's own capability list as its input.
 *
 * Negotiation is the capability list first and the api integer second. The
 * integer says which contract this is; the list says which parts of it this
 * particular build compiled. A firmware built without the push-to-talk path
 * reports api 2 and omits `voice.ptt.v1`, and the Voice page has to be able to
 * tell that apart from a device that is merely muted.
 */

export type SettingAccess =
  | "remote-rw"
  | "remote-write-only"
  | "remote-action"
  | "physical-only"
  | "read-only";

export type SettingTransport = "v1" | "v2" | "none";

/** How a written value actually takes effect, in the device's own words. */
export type ApplyMode =
  | "immediate"
  | "immediate_not_persisted"
  | "restart_required";

/** The capability strings the firmware advertises on the status route. */
export const CAP_CONFIG_V2 = "config.v2";
export const CAP_ACTION_RESTART = "action.restart";
export const CAP_ACTION_SLEEP = "action.sleep";
export const CAP_VOICE_HUB_V1 = "voice.hub.v1";
export const CAP_VOICE_PTT_V1 = "voice.ptt.v1";
export const CAP_FRAME_V1 = "dashboard.frame.v1";

export interface SettingEntry {
  key: string;
  /** Dotted wire name for a v2 field, absent for actions and info rows. */
  field?: string;
  label: string;
  section: "System" | "Gallery" | "Network" | "Dashboard" | "Voice";
  type: "boolean" | "enum" | "integer" | "string" | "action" | "info";
  constraints?: {
    values?: readonly (string | number)[];
    min?: number;
    max?: number;
    unit?: string;
  };
  access: SettingAccess;
  transport: SettingTransport;
  /** The capability that has to be present for this row to be live. */
  capability?: string;
  /** Typed confirmation word required before this write or action runs. */
  confirmGate?: string;
  /**
   * The confirmation literal the DEVICE requires in the patch body. Distinct
   * from confirmGate, which is what the user types into the tower: one guards
   * a human, the other guards a wire.
   */
  deviceConfirm?: string;
  /** Only this direction needs the gate. Undefined means both do. */
  confirmWhenValueIs?: boolean;
  /** What the firmware will do with a write, before it is attempted. */
  applyMode?: ApplyMode;
  /**
   * True when the tower may raise this flag but never lower it. Lockdown is
   * one way by design: turning the legacy write routes back on is a decision
   * for somebody holding the device.
   */
  oneWayTo?: boolean;
  /** Where to change it on the device when the tower cannot. */
  onDevicePath?: string;
  /** File and line the claim comes from. Shown as a provenance tooltip. */
  evidence: string;
  note?: string;
}

/** The one copy string for every row that firmware cannot expose yet. */
export function gatedTooltip(entry: SettingEntry): string {
  const where = entry.onDevicePath ?? entry.label;
  return `Remote control of this setting needs the firmware config API, which this device does not report. Until then, change it on the device: ${where}.`;
}

/** The one copy string for each way a write takes effect. */
export const APPLY_MODE_COPY: Record<ApplyMode, string> = {
  immediate: "takes effect now and survives a restart",
  immediate_not_persisted:
    "takes effect now, and is back to its default after a restart",
  restart_required: "stored now, in force after a restart",
};

export const APPLY_MODE_BADGE: Record<ApplyMode, string> = {
  immediate: "immediate",
  immediate_not_persisted: "immediate, not saved",
  restart_required: "restart required",
};

export const SETTINGS_REGISTRY: readonly SettingEntry[] = [
  {
    key: "system.restart",
    label: "Restart",
    section: "System",
    type: "action",
    access: "remote-action",
    transport: "v2",
    capability: CAP_ACTION_RESTART,
    confirmGate: "RESTART",
    deviceConfirm: "restart",
    onDevicePath: "Settings > Restart",
    evidence:
      "POST /api/v1/actions/restart, confirmation literal plus Idempotency-Key, executed after a 1 s delay so the response flushes",
    note: "A restart interrupts nothing that is stored. The panel keeps whatever image it was last given, because e-paper holds its image with the power off.",
  },
  {
    key: "system.sleep",
    label: "Enter deep sleep",
    section: "System",
    type: "action",
    access: "remote-action",
    transport: "v2",
    capability: CAP_ACTION_SLEEP,
    confirmGate: "SLEEP",
    deviceConfirm: "sleep",
    onDevicePath: "Settings > Power saving",
    evidence:
      "POST /api/v1/actions/sleep. The device stops the LAN service and Wi-Fi before sleeping",
    note: "The device leaves the network. Only the physical BOOT button wakes it, so the tower cannot bring it back.",
  },
  {
    key: "system.sync_interval",
    field: "sync.sync_interval",
    label: "Idle sleep after",
    section: "System",
    type: "integer",
    constraints: { min: 0, max: 1440, unit: "minutes" },
    access: "remote-rw",
    transport: "v2",
    capability: CAP_CONFIG_V2,
    applyMode: "immediate",
    onDevicePath: "Settings > Power saving",
    evidence:
      "sync/sync_interval in NVS, default 30 minutes. 0 turns idle deep sleep off",
    note: "Zero means the device never sleeps on its own. A sleeping device is not reachable, so every minute here is a minute the tower can push.",
  },
  {
    key: "gallery.slide_min",
    field: "gallery.slide_min",
    label: "Slideshow interval",
    section: "Gallery",
    type: "enum",
    constraints: { values: [0, 5, 10, 30], unit: "minutes" },
    access: "remote-rw",
    transport: "v2",
    capability: CAP_CONFIG_V2,
    applyMode: "immediate",
    onDevicePath: "Settings > Slideshow interval",
    evidence: "gallery/slide_min in NVS, default 5, and the device menu cycles exactly these four values",
    note: "Zero turns the slideshow off. While it is on, the device refuses to idle sleep, because a device that slept between slides would show one photo.",
  },
  {
    key: "network.wifi",
    label: "Wi-Fi credentials",
    section: "Network",
    type: "info",
    access: "physical-only",
    transport: "none",
    onDevicePath: "the physical setup portal (UP and DOWN long press)",
    evidence:
      "Credentials live in the vendored esp-wifi-connect component and are only settable through the AP portal",
    note: "Out of scope for the config API by design, not by omission. The device reports wifi_writable: false so no client has to guess.",
  },
  {
    key: "network.lan_service",
    field: "network.lan_service",
    label: "LAN service",
    section: "Network",
    type: "boolean",
    access: "remote-rw",
    transport: "v2",
    capability: CAP_CONFIG_V2,
    applyMode: "immediate_not_persisted",
    confirmGate: "LAN OFF",
    deviceConfirm: "lan_service_off",
    confirmWhenValueIs: false,
    onDevicePath: "Settings > LAN service",
    evidence: "Runtime-only state in application.cc. It is not written to NVS, and it restarts with Wi-Fi",
    note: "Turning this off severs the API the tower is talking through. Only a button press on the device, or the next Wi-Fi reconnection, brings it back.",
  },
  {
    key: "network.lan_address",
    label: "LAN address",
    section: "Network",
    type: "string",
    access: "read-only",
    transport: "none",
    evidence: "Tower configuration; the device shows the same value read-only in Settings",
  },
  {
    key: "dashboard.pairing",
    label: "Pairing",
    section: "Dashboard",
    type: "info",
    access: "read-only",
    transport: "v1",
    capability: CAP_FRAME_V1,
    onDevicePath: "Settings > Pair dashboard",
    evidence:
      "POST /api/v1/dashboard/pair claims a token during a physically opened 120 second window (docs/PROVISIONING.md)",
    note: "Re-pairing mints a new token and would invalidate the composer bridge's credential. The tower never initiates it.",
  },
  {
    key: "dashboard.lockdown",
    field: "dashboard.lockdown",
    label: "Legacy writes blocked",
    section: "Dashboard",
    type: "boolean",
    access: "remote-rw",
    transport: "v2",
    capability: CAP_CONFIG_V2,
    applyMode: "immediate",
    oneWayTo: true,
    onDevicePath: "Settings > Block legacy writes",
    evidence:
      "common/dashboard_manager.cc defaults lockdown on; the config API accepts true and refuses false with 403",
    note: "One way. The tower can block the unauthenticated gallery routes but can never unblock them: that takes a button press on the device.",
  },
  {
    key: "voice.hub",
    label: "Voice hub URL and token",
    section: "Voice",
    type: "string",
    access: "remote-write-only",
    transport: "v1",
    capability: CAP_VOICE_HUB_V1,
    evidence: "POST /api/v1/voice/hub carries the token; PATCH /api/v1/config can change only the URL",
    note: "Write only. The device never echoes the token back, and neither does the tower.",
  },
  {
    key: "voice.muted",
    field: "voice.muted",
    label: "Mute voice",
    section: "Voice",
    type: "boolean",
    access: "remote-rw",
    transport: "v2",
    capability: CAP_CONFIG_V2,
    applyMode: "immediate",
    onDevicePath: "Settings > Mute voice",
    evidence: "voice/muted in NVS, default true. Written before it is applied, so a power cut cannot unmute",
    note: "Software mute, and the firmware label says so. Unmuting does not start a recording: push to talk also needs the capture path compiled in, Wi-Fi up and a hub configured.",
  },
] as const;

export interface DeviceCapabilities {
  api: number;
  capabilities: readonly string[];
}

/** Read the negotiation signals off a status response. */
export function negotiate(status: {
  api?: number;
  capabilities?: readonly string[] | null;
}): DeviceCapabilities {
  return {
    api: typeof status.api === "number" ? status.api : 1,
    capabilities: status.capabilities ?? [],
  };
}

export function hasCapability(
  device: DeviceCapabilities,
  capability: string,
): boolean {
  return device.capabilities.includes(capability);
}

/**
 * Is this row live against this device?
 *
 * A v1 row is live at api 1 even when the capability list is empty, because
 * an api 1 firmware does not publish one and its six routes are known. A v2
 * row needs both the api level and the named capability: a build that omitted
 * a feature reports api 2 and leaves it out of the list.
 */
export function isLive(entry: SettingEntry, device: DeviceCapabilities): boolean {
  if (entry.transport === "none") return false;
  if (entry.transport === "v1") {
    if (device.api < 1) return false;
    if (device.api >= 2 && entry.capability) {
      return hasCapability(device, entry.capability);
    }
    return true;
  }
  if (device.api < 2) return false;
  return entry.capability ? hasCapability(device, entry.capability) : true;
}

/** Rows the user can actually change from here. */
export function isEditable(
  entry: SettingEntry,
  device: DeviceCapabilities,
): boolean {
  if (!isLive(entry, device)) return false;
  return entry.access === "remote-rw" || entry.access === "remote-write-only";
}

/** Rows that exist on the device but cannot be reached from here. */
export function isGated(
  entry: SettingEntry,
  device: DeviceCapabilities,
): boolean {
  if (entry.access === "read-only") return false;
  return !isLive(entry, device);
}

export function entryByKey(key: string): SettingEntry | undefined {
  return SETTINGS_REGISTRY.find((entry) => entry.key === key);
}

/** The registry rows that map to a writable dotted field. */
export function writableFields(device: DeviceCapabilities): SettingEntry[] {
  return SETTINGS_REGISTRY.filter(
    (entry) => entry.field !== undefined && isEditable(entry, device),
  );
}

/**
 * Does a proposed value need the user to type a confirmation word?
 *
 * Two rows need one and for different reasons: turning the LAN service off
 * cuts the tower's own connection, and there is no undo for either from here.
 */
export function needsConfirmation(
  entry: SettingEntry,
  value: number | boolean | string,
): boolean {
  if (!entry.confirmGate) return false;
  if (entry.confirmWhenValueIs === undefined) return true;
  return value === entry.confirmWhenValueIs;
}

export interface AboutField {
  key: string;
  label: string;
  value: string;
  /** Provenance, shown as a tooltip on every field. */
  tooltip: string;
}

/**
 * About rows, read from the device rather than written here.
 *
 * The previous version of this function returned hardcoded vendor strings
 * ("notellm", "Youn-Beta1.0") because api 1 reported none. Those are gone: a
 * device that does not report a value gets "not reported", which is a true
 * statement, where a vendor name inherited from an upstream project was not.
 */
export function aboutFields(status: {
  firmware?: string;
  api?: number;
  capabilities?: readonly string[] | null;
  device?: {
    name?: string;
    model?: string;
    hardware?: string;
    panel?: string;
    fw?: string;
    upstream_base?: string;
  } | null;
  config_revision?: number | null;
}): AboutField[] {
  const meta = status.device ?? null;
  const unreported = "not reported";
  const fields: AboutField[] = [
    {
      key: "device_name",
      label: "Device",
      value: meta?.name ?? unreported,
      tooltip:
        "The product name the device reports about itself. At api 1 the device reported no name at all, and the tower showed the upstream vendor's.",
    },
    {
      key: "firmware",
      label: "Firmware",
      value: meta?.fw ?? status.firmware ?? unreported,
      tooltip:
        "The version of this firmware, which moves when this firmware moves. Nothing in the tower decides what it can do from this string: that is what the capability list is for.",
    },
    {
      key: "hardware",
      label: "Hardware",
      value: meta?.hardware ?? unreported,
      tooltip:
        "The panel family and its colour count, as the device reports them.",
    },
    {
      key: "model",
      label: "Board",
      value: meta?.model ?? unreported,
      tooltip: "The board, as named in the firmware's own boards directory.",
    },
    {
      key: "panel",
      label: "Panel",
      value: meta?.panel ?? "400x300, four colours",
      tooltip:
        "Black, white, yellow and red, in that bit order. A refresh develops four pigments and takes tens of seconds; no firmware change shortens it.",
    },
    {
      key: "api",
      label: "Device API",
      value: String(status.api ?? 1),
      tooltip:
        "Which contract the device speaks. The capability list below says which parts of it this build actually compiled.",
    },
    {
      key: "capabilities",
      label: "Capabilities",
      value:
        status.capabilities && status.capabilities.length > 0
          ? status.capabilities.join(", ")
          : unreported,
      tooltip:
        "Reported by the device. Every remote control on this page is gated on one of these strings, so a build without a feature cannot render a control for it.",
    },
  ];

  if (typeof status.config_revision === "number") {
    fields.push({
      key: "config_revision",
      label: "Config revision",
      value: String(status.config_revision),
      tooltip:
        "Bumped by every settings change, including ones made by pressing buttons on the device. The tower sends it back with a write so a change it never saw cannot be silently overwritten.",
    });
  }

  if (meta?.upstream_base) {
    fields.push({
      key: "upstream_base",
      label: "Upstream base",
      value: meta.upstream_base,
      tooltip:
        "The vendor firmware version this project was forked from. A fact about provenance, and not a version of this firmware.",
    });
  }

  return fields;
}
