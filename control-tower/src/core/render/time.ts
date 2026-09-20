/**
 * The timezone the panel's clock and dates are rendered in.
 *
 * It used to be one city, written here as a constant, which made every panel
 * in the world show Eastern time. It now resolves, in order: the
 * NOTE4C_PANEL_TIMEZONE variable, this machine's own zone, and UTC.
 *
 * Resolved once at module load rather than per call because a panel's timezone
 * does not change while a process runs, and because this constant is read by
 * the browser preview as well as the server. Both run on the same machine for
 * a local-only product, so with the variable unset they agree by construction.
 * Setting it for the server alone would make the preview and the panel differ
 * by an offset — which is why the variable is documented as "set it for both,
 * or leave it unset" in .env.example.
 */
function resolvePanelTimeZone(): string {
  const system = Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
  // `process` is absent in a browser bundle for anything not prefixed with
  // NEXT_PUBLIC_, so this reads as undefined there rather than throwing.
  const configured =
    typeof process !== "undefined"
      ? (process.env?.NOTE4C_PANEL_TIMEZONE ?? "").trim()
      : "";
  if (configured.length === 0) return system;
  try {
    new Intl.DateTimeFormat("en-CA", { timeZone: configured }).format(new Date());
    return configured;
  } catch {
    return system;
  }
}

export const PANEL_TIMEZONE = resolvePanelTimeZone();

/**
 * The panel's location, for the astronomy the Sky module computes on the Mac.
 *
 * Resolved from NOTE4C_WEATHER_LATITUDE / NOTE4C_WEATHER_LONGITUDE — the same
 * pair the weather source already reads, so a panel configured for a place
 * gets a sky for that place with no second setting — and falling back to Paris
 * when unset, because a sun arc needs a somewhere and a blank tile teaches
 * nothing. Read once at module load, like the timezone, and with the same
 * caveat: the browser preview reads a bundle that does not carry a non-public
 * env var, so it falls back to Paris. Set it for both processes or leave it
 * unset, exactly as with NOTE4C_PANEL_TIMEZONE. The Sky module also exposes
 * latitude and longitude as its own options, so any panel can be corrected in
 * the inspector regardless of the environment.
 */
export const DEFAULT_LATITUDE = 48.8566;
export const DEFAULT_LONGITUDE = 2.3522;

function envNumber(name: string): number | null {
  if (typeof process === "undefined") return null;
  const raw = (process.env?.[name] ?? "").trim();
  if (raw.length === 0) return null;
  const value = Number(raw);
  return Number.isFinite(value) ? value : null;
}

function resolvePanelLatitude(): number {
  const value = envNumber("NOTE4C_WEATHER_LATITUDE");
  return value !== null && Math.abs(value) <= 90 ? value : DEFAULT_LATITUDE;
}

function resolvePanelLongitude(): number {
  const value = envNumber("NOTE4C_WEATHER_LONGITUDE");
  return value !== null && Math.abs(value) <= 180 ? value : DEFAULT_LONGITUDE;
}

export const PANEL_LATITUDE = resolvePanelLatitude();
export const PANEL_LONGITUDE = resolvePanelLongitude();

/**
 * The panel timestamp: "dd/mm HH:MM" in the panel's own timezone, matching
 * the composer's strftime('%d/%m %H:%M'). Intl is used rather than a date
 * library so the result is identical in Node and in the browser preview.
 */
export function formatPanelStamp(
  date: Date,
  timeZone: string = PANEL_TIMEZONE,
): string {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone,
    day: "2-digit",
    month: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    hourCycle: "h23",
  }).formatToParts(date);

  const pick = (type: Intl.DateTimeFormatPartTypes): string =>
    parts.find((part) => part.type === type)?.value ?? "00";

  return `${pick("day")}/${pick("month")} ${pick("hour")}:${pick("minute")}`;
}

/** Local hour label used by the weather slots, e.g. "07h". */
export function formatHourLabel(
  date: Date,
  timeZone: string = PANEL_TIMEZONE,
): string {
  const hour = new Intl.DateTimeFormat("en-CA", {
    timeZone,
    hour: "2-digit",
    hourCycle: "h23",
  }).formatToParts(date);
  return `${hour.find((part) => part.type === "hour")?.value ?? "00"}h`;
}

/** Day and month only, e.g. "12/09". */
export function formatDayMonth(
  date: Date,
  timeZone: string = PANEL_TIMEZONE,
): string {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone,
    day: "2-digit",
    month: "2-digit",
  }).formatToParts(date);
  const pick = (type: Intl.DateTimeFormatPartTypes): string =>
    parts.find((part) => part.type === type)?.value ?? "00";
  return `${pick("day")}/${pick("month")}`;
}

/** Local date and time, e.g. "12/09 08:30". */
export function formatEventWhen(
  date: Date,
  timeZone: string = PANEL_TIMEZONE,
): string {
  return formatPanelStamp(date, timeZone);
}
