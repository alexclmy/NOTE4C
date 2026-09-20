/**
 * Astronomy, in closed form, on the Mac.
 *
 * The Sky module needs sunrise, sunset, day length, its day-over-day change,
 * where the sun is on its arc right now, and the moon's phase. None of that is
 * a network fact — it is geometry, computable from a latitude, a longitude and
 * the clock — so none of it is fetched. This module is pure TypeScript with no
 * clock of its own: every function takes the instant it should answer for, so
 * the browser preview and the packed device bytes agree to the pixel.
 *
 * The sunrise maths is the standard NOAA sunrise equation (see Wikipedia,
 * "Sunrise equation"); the moon phase is the standard synodic-month
 * approximation from a known new moon. Both are good to about a minute and a
 * couple of percent respectively, which is well inside what a panel that
 * repaints a few times a day can show.
 */

const RAD = Math.PI / 180;
const J2000 = 2451545.0;
/** Julian date of the Unix epoch. */
const JD_UNIX_EPOCH = 2440587.5;
const MS_PER_DAY = 86_400_000;

function toJulian(date: Date): number {
  return date.getTime() / MS_PER_DAY + JD_UNIX_EPOCH;
}

function fromJulian(jd: number): Date {
  return new Date((jd - JD_UNIX_EPOCH) * MS_PER_DAY);
}

export type SunKind = "normal" | "polar-day" | "polar-night";

export interface SunDay {
  kind: SunKind;
  /** Solar noon for the day. Always defined, even in the polar cases. */
  transit: Date;
  /** Undefined when the sun does not cross the horizon on this day. */
  sunrise?: Date;
  sunset?: Date;
  /** Minutes the sun is above the horizon. 0 or 1440 in the polar cases. */
  dayLengthMinutes: number;
}

/**
 * Sunrise, sunset and solar noon for the civil day that `date` falls in, at the
 * given latitude and longitude (north and east positive).
 */
export function sunDay(date: Date, latitude: number, longitude: number): SunDay {
  const lw = -longitude; // the algorithm is written in west-positive longitude
  const phi = latitude * RAD;

  const jd = toJulian(date);
  // Which solar cycle today is, corrected for longitude.
  const n = Math.round(jd - J2000 - 0.0009 + lw / 360);
  const jStar = J2000 + 0.0009 + lw / 360 + n; // approximate solar noon
  const M = (357.5291 + 0.98560028 * (jStar - J2000)) % 360; // mean anomaly, deg
  const Mrad = M * RAD;
  const C =
    1.9148 * Math.sin(Mrad) + 0.02 * Math.sin(2 * Mrad) + 0.0003 * Math.sin(3 * Mrad);
  const lambda = ((M + C + 180 + 102.9372) % 360) * RAD; // ecliptic longitude
  const jTransit =
    jStar + 0.0053 * Math.sin(Mrad) - 0.0069 * Math.sin(2 * lambda);
  const delta = Math.asin(Math.sin(lambda) * Math.sin(23.4397 * RAD)); // declination

  const h0 = -0.833 * RAD; // sun's centre this far below true horizon at rise/set
  const cosOmega =
    (Math.sin(h0) - Math.sin(phi) * Math.sin(delta)) /
    (Math.cos(phi) * Math.cos(delta));

  const transit = fromJulian(jTransit);
  if (cosOmega > 1) {
    return { kind: "polar-night", transit, dayLengthMinutes: 0 };
  }
  if (cosOmega < -1) {
    return { kind: "polar-day", transit, dayLengthMinutes: 1440 };
  }
  const omega = Math.acos(cosOmega) / RAD; // hour angle, degrees
  const sunrise = fromJulian(jTransit - omega / 360);
  const sunset = fromJulian(jTransit + omega / 360);
  const dayLengthMinutes = (sunset.getTime() - sunrise.getTime()) / 60_000;
  return { kind: "normal", transit, sunrise, sunset, dayLengthMinutes };
}

/** Day length in minutes for the civil day `date` falls in. */
export function dayLengthMinutes(
  date: Date,
  latitude: number,
  longitude: number,
): number {
  return sunDay(date, latitude, longitude).dayLengthMinutes;
}

/**
 * How much longer or shorter today's daylight is than yesterday's, in whole
 * minutes. Positive when the days are drawing out. This is the number that
 * makes a sky panel feel like it is about the season and not just the day.
 */
export function dayLengthDeltaMinutes(
  date: Date,
  latitude: number,
  longitude: number,
): number {
  const today = dayLengthMinutes(date, latitude, longitude);
  const yesterday = dayLengthMinutes(
    new Date(date.getTime() - MS_PER_DAY),
    latitude,
    longitude,
  );
  return Math.round(today - yesterday);
}

export interface SunPosition {
  /** True when the sun is currently above the horizon. */
  up: boolean;
  /**
   * Fraction of the daylight arc travelled, 0 at sunrise, 1 at sunset. Clamped,
   * so before dawn it reads 0 and after dusk 1.
   */
  arc: number;
  /**
   * Normalised height on the arc, 0 at the horizon and 1 at solar noon, as a
   * simple sine of the arc fraction. What the drawing uses to place the disc.
   */
  altitude: number;
}

/**
 * Where the sun is on today's arc at instant `now`. Pure geometry over the
 * day's own sunrise and sunset, so a caller that froze the clock for hashing
 * gets a frozen position and a caller drawing for real gets the live one.
 */
export function sunPosition(now: Date, day: SunDay): SunPosition {
  if (day.kind === "polar-day") return { up: true, arc: 0.5, altitude: 1 };
  if (day.kind === "polar-night" || !day.sunrise || !day.sunset) {
    return { up: false, arc: 0, altitude: 0 };
  }
  const start = day.sunrise.getTime();
  const end = day.sunset.getTime();
  const t = now.getTime();
  const raw = (t - start) / (end - start);
  const arc = raw < 0 ? 0 : raw > 1 ? 1 : raw;
  return {
    up: t >= start && t <= end,
    arc,
    altitude: Math.sin(arc * Math.PI),
  };
}

export type MoonPhaseName =
  | "new"
  | "waxing-crescent"
  | "first-quarter"
  | "waxing-gibbous"
  | "full"
  | "waning-gibbous"
  | "last-quarter"
  | "waning-crescent";

export interface MoonPhase {
  /** 0 at new, 0.5 at full, wrapping back to new at 1. */
  phase: number;
  /** Illuminated fraction of the disc, 0 at new, 1 at full. */
  illumination: number;
  /** True from new through full (the lit limb is on the right, northern sky). */
  waxing: boolean;
  name: MoonPhaseName;
}

const SYNODIC_MONTH = 29.530588853;
/** A reference new moon: 2000-01-06 18:14 UTC, in Julian days. */
const REFERENCE_NEW_MOON = 2451550.26;

/** The moon's phase at instant `date`. */
export function moonPhase(date: Date): MoonPhase {
  const jd = toJulian(date);
  let phase = ((jd - REFERENCE_NEW_MOON) / SYNODIC_MONTH) % 1;
  if (phase < 0) phase += 1;
  const illumination = 0.5 * (1 - Math.cos(2 * Math.PI * phase));
  const waxing = phase < 0.5;
  return { phase, illumination, waxing, name: moonPhaseName(phase) };
}

function moonPhaseName(phase: number): MoonPhaseName {
  // Eighth-of-a-month bands, with the four principal phases given a narrow
  // window of their own so "full" means full and not "gibbous, nearly".
  const p = phase;
  if (p < 0.03 || p >= 0.97) return "new";
  if (p < 0.22) return "waxing-crescent";
  if (p < 0.28) return "first-quarter";
  if (p < 0.47) return "waxing-gibbous";
  if (p < 0.53) return "full";
  if (p < 0.72) return "waning-gibbous";
  if (p < 0.78) return "last-quarter";
  return "waning-crescent";
}

export const MOON_PHASE_LABEL: Record<MoonPhaseName, string> = {
  new: "Nouvelle lune",
  "waxing-crescent": "Premier croissant",
  "first-quarter": "Premier quartier",
  "waxing-gibbous": "Gibbeuse croissante",
  full: "Pleine lune",
  "waning-gibbous": "Gibbeuse décroissante",
  "last-quarter": "Dernier quartier",
  "waning-crescent": "Dernier croissant",
};

/**
 * "HH:MM" for an instant in a given IANA timezone, using Intl so Node and the
 * browser preview format identically. Kept here so the Sky module has one way
 * to print a time and does not reinvent the 24-hour clock.
 */
export function formatClock(date: Date, timeZone: string): string {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone,
    hour: "2-digit",
    minute: "2-digit",
    hourCycle: "h23",
  }).formatToParts(date);
  const pick = (type: Intl.DateTimeFormatPartTypes): string =>
    parts.find((part) => part.type === type)?.value ?? "00";
  return `${pick("hour")}:${pick("minute")}`;
}

/** "12 h 34" for a duration in minutes; the panel's own way of saying a span. */
export function formatDuration(totalMinutes: number): string {
  const minutes = Math.max(0, Math.round(totalMinutes));
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  return `${h} h ${m.toString().padStart(2, "0")}`;
}
