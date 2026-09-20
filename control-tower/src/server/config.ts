/**
 * Everything this tower learns about the machine it is installed on.
 *
 * One file, because the alternative is what this project had until now: a
 * `/Users/…` path in `gates.ts`, a household's calendar name inside a zod
 * schema, a Homebrew prefix in the Reminders adapter, one city's coordinates
 * in the weather source, and a LAN address as a constant in the state store.
 * All of it correct for exactly one computer and wrong, silently, for every
 * other — which is a defect the moment anybody else runs it.
 *
 * Three rules hold here:
 *
 *  1. **Nothing is assumed.** An unset variable means the feature is not
 *     configured, and the source that needs it says `not configured` instead
 *     of reaching for a file that does not exist or fetching a forecast for a
 *     city nobody chose. There are no fallback defaults pointing at one
 *     person's machine.
 *  2. **Read at call time, never at import.** A constant captured at module
 *     load cannot be changed by a test, and the tests here set and unset these
 *     variables case by case.
 *  3. **These are locations, not secrets.** Paths and origins may be shown in
 *     the interface — the operator needs to know which file the tower is about
 *     to read. The *contents* of those files (tokens, credentials) never leave
 *     the server and are never logged; see src/server/device/token.ts and
 *     src/server/sources/haSensor.ts.
 *
 * Every variable below is listed in `.env.example` with the same wording.
 */

function env(name: string): string {
  const value = process.env[name];
  return typeof value === "string" ? value.trim() : "";
}

/**
 * Where the composer bridge's device token can be copied from.
 *
 * Unset by default: importing a credential from another program's directory is
 * an operation most installations have no reason to offer, and the button that
 * would do it stays hidden until this names a file.
 */
export function bridgeTokenPath(): string {
  return env("NOTE4C_BRIDGE_TOKEN_PATH");
}

/** The EventKit worker's snapshot file. Empty means "no calendar source". */
export function calendarSnapshotPath(): string {
  return env("NOTE4C_CALENDAR_SNAPSHOT_PATH");
}

/**
 * The calendar the snapshot must be of, or null for "whatever it says".
 *
 * This used to be a literal inside the zod schema, which made one household's
 * calendar name a structural requirement of the file format. Now it is an
 * optional assertion: set it and a snapshot of the wrong calendar is rejected
 * rather than rendered, leave it and any snapshot is accepted.
 */
export function calendarName(): string | null {
  const value = env("NOTE4C_CALENDAR_NAME");
  return value.length > 0 ? value : null;
}

/**
 * The `remindctl` binary. Empty means the Reminders source is not configured.
 *
 * An absolute path on purpose, and never a PATH lookup: this is spawned with
 * no shell, and resolving a bare name through the environment is how the wrong
 * binary gets run.
 */
export function remindctlBin(): string {
  return env("NOTE4C_REMINDCTL_BIN");
}

/**
 * A dotenv file holding HASS_URL and HASS_TOKEN.
 *
 * The credentials are read per request and never copied into the tower's own
 * store. `HASS_URL`/`HASS_TOKEN` set directly in the process environment work
 * too, and take precedence: see readCredentials.
 */
export function homeAssistantEnvPath(): string {
  return env("NOTE4C_HA_ENV_PATH");
}

/**
 * A second, separate renderer this tower may read and never writes to.
 *
 * Very specific to the installation this project grew out of, so it is off
 * unless named. When it is empty the card disappears from Overview and
 * Diagnostics rather than reporting "not running" forever about a program
 * nobody has.
 */
export function composerOrigin(): string {
  return env("NOTE4C_COMPOSER_ORIGIN");
}

export interface WeatherLocation {
  latitude: number;
  longitude: number;
  /** What the panel prints beside the numbers. Provenance, not decoration. */
  label: string;
}

/**
 * Where the forecast is for, or null when nobody has said.
 *
 * Both halves or neither: a latitude with no longitude is not a location, and
 * defaulting the other half would put the weather somewhere on the same
 * parallel as the user and nowhere near them. Out-of-range values are refused
 * for the same reason — a source that quietly clamps 91° to 90° is inventing a
 * place.
 */
export function weatherLocation(): WeatherLocation | null {
  const rawLat = env("NOTE4C_WEATHER_LATITUDE");
  const rawLon = env("NOTE4C_WEATHER_LONGITUDE");
  if (rawLat.length === 0 || rawLon.length === 0) return null;

  const latitude = Number(rawLat);
  const longitude = Number(rawLon);
  if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) return null;
  if (Math.abs(latitude) > 90 || Math.abs(longitude) > 180) return null;

  const label = env("NOTE4C_WEATHER_LABEL");
  return {
    latitude,
    longitude,
    label:
      label.length > 0
        ? label
        : `${latitude.toFixed(2)}, ${longitude.toFixed(2)}`,
  };
}

/**
 * The timezone the panel's clock and dates are rendered in.
 *
 * Defaults to this machine's own zone, which is right for a tower and a
 * browser running on the same computer — the usual case for a local-only
 * product. An IANA name this runtime cannot resolve is ignored rather than
 * allowed to throw inside Intl on every render, because the failure mode of a
 * bad timezone is a blank tile on a device that repaints once an hour.
 *
 * Note for anyone setting this: the browser preview reads its own default the
 * same way, so an override applied only to the server process can make the
 * preview and the panel disagree. Set it for both, or leave it unset.
 */
export function panelTimeZone(): string {
  const system = Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
  const configured = env("NOTE4C_PANEL_TIMEZONE");
  if (configured.length === 0) return system;
  try {
    new Intl.DateTimeFormat("en-CA", { timeZone: configured }).format(new Date());
    return configured;
  } catch {
    return system;
  }
}

/**
 * The device address a fresh tower starts with.
 *
 * Empty, so a new installation has nothing to talk to until somebody types an
 * address — which is exactly the state the onboarding describes. It used to be
 * one real panel's LAN address, which meant every clone of this repository
 * pointed at a stranger's network by default.
 */
export function defaultDeviceAddress(): string {
  return env("NOTE4C_DEVICE_ADDRESS");
}
