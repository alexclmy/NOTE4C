import { z } from "zod";
import type { WeatherValue } from "@/core/render/data";
import { formatHourLabel } from "@/core/render/time";
import type { ModuleData } from "@/core/render/types";
import { weatherLocation } from "@/server/config";
import { TOWER_USER_AGENT } from "@/core/version";
import { SourceError, fetchJson } from "./http";
import { notConfigured } from "./notConfigured";

/**
 * The forecast URL for the configured location, or null when there is none.
 *
 * The coordinates used to be two constants naming one city — approximate city
 * centre, deliberately never a home address — which is the right *shape* of
 * decision and the wrong place to record it. They are now
 * NOTE4C_WEATHER_LATITUDE and NOTE4C_WEATHER_LONGITUDE, with no default: an
 * installation that has not said where it is gets "not configured" rather than
 * a stranger's weather.
 *
 * Deliberately coarse is still the rule, and it belongs in the docs beside the
 * variable: a panel on a wall needs the weather over a city, not over a house,
 * and a coordinate pair in a config file is a coordinate pair in a backup.
 */
export function openMeteoUrl(): string | null {
  const location = weatherLocation();
  if (location === null) return null;
  return (
    `https://api.open-meteo.com/v1/forecast?latitude=${location.latitude}` +
    `&longitude=${location.longitude}` +
    "&hourly=temperature_2m,weather_code" +
    "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
    "&timezone=UTC&forecast_days=7"
  );
}

/**
 * An optional second forecast source, for when the first one is down.
 *
 * It is a plain URL in NOTE4C_WEATHER_FALLBACK_URL because the one this
 * project was built against — Environment and Climate Change Canada's city
 * page feed — is a national service with a per-city identifier, and there is
 * no general way to derive the right one from a latitude and a longitude. An
 * installation outside Canada leaves it unset and has one source; an
 * installation inside it sets its own city's URL. The response shape is the
 * ECCC one; anything else is reported as unreadable rather than guessed at.
 */
export function fallbackForecastUrl(): string {
  const value = process.env.NOTE4C_WEATHER_FALLBACK_URL;
  return typeof value === "string" ? value.trim() : "";
}

const EcccResponse = z.object({
  properties: z.object({
    hourlyForecastGroup: z.object({
      hourlyForecasts: z.array(
        z.object({
          timestamp: z.string(),
          condition: z.object({ en: z.string().optional() }).optional(),
          temperature: z.object({
            value: z.object({ en: z.number().nullable() }),
          }),
        }),
      ),
    }),
  }),
});

const OpenMeteoResponse = z.object({
  hourly: z.object({
    time: z.array(z.string()),
    temperature_2m: z.array(z.number().nullable()),
    weather_code: z.array(z.number().nullable()),
  }),
  // The daily block is optional so the hourly-only contract still parses: a
  // response without it (or the ECCC fallback) simply yields no multi-day
  // outlook, and the module that wants one renders its unavailable state.
  daily: z
    .object({
      time: z.array(z.string()),
      weather_code: z.array(z.number().nullable()),
      temperature_2m_max: z.array(z.number().nullable()),
      temperature_2m_min: z.array(z.number().nullable()),
    })
    .optional(),
});

/** Uppercase weekday label for a calendar date, e.g. "2026-09-22" → "TUE". */
const WEEKDAY = new Intl.DateTimeFormat("en-US", {
  weekday: "short",
  timeZone: "UTC",
});
function weekdayLabel(dateStamp: string): string {
  // A daily "time" is a bare calendar date; read it at noon UTC so the weekday
  // is unambiguous, then upper-case the short name the way the strip prints it.
  const at = new Date(`${dateStamp}T12:00:00Z`);
  if (Number.isNaN(at.getTime())) return "";
  return WEEKDAY.format(at).toUpperCase();
}

/**
 * The multi-day outlook, one entry per day the API returned. A day is kept only
 * when it has both a max and a min — a half-populated day is worse than one
 * fewer column, and never a zero standing in for a missing reading.
 */
export function parseOpenMeteoDaily(
  payload: unknown,
  limit = 7,
): import("@/core/render/data").DayForecast[] {
  const parsed = OpenMeteoResponse.safeParse(payload);
  if (!parsed.success || !parsed.data.daily) return [];
  const { time, weather_code: codes, temperature_2m_max: highs, temperature_2m_min: lows } =
    parsed.data.daily;

  const days: import("@/core/render/data").DayForecast[] = [];
  for (let i = 0; i < time.length && days.length < limit; i += 1) {
    const stamp = time[i];
    const high = highs[i];
    const low = lows[i];
    if (stamp === undefined || high === null || high === undefined) continue;
    if (low === null || low === undefined) continue;
    const label = weekdayLabel(stamp);
    if (label.length === 0) continue;
    days.push({
      label,
      condition: conditionForWmoCode(codes[i] ?? -1),
      high: Math.round(high),
      low: Math.round(low),
    });
  }
  return days;
}

/** WMO code mapping, identical to the composer's condition() lookup. */
export function conditionForWmoCode(code: number): string {
  if (code === 0) return "sunny";
  if (code === 1 || code === 2) return "partlycloudy";
  if (code === 3) return "cloudy";
  if (code === 45 || code === 48) return "fog";
  if ([71, 73, 75, 77, 85, 86].includes(code)) return "snowy";
  if ([95, 96, 99].includes(code)) return "lightning-rainy";
  if ([51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 80, 81, 82].includes(code)) {
    return "rainy";
  }
  return "unknown";
}

export interface HourlyRow {
  at: Date;
  temperature: number;
  condition: string;
}

const HOUR_MS = 3_600_000;

/**
 * The composer's coverage contract, ported exactly: at least 23 hourly
 * entries inside the next 24 hours, the first no more than an hour away, the
 * last at least 23 hours out, and no gap wider than an hour. A partial
 * forecast is worse than none: it looks complete on frozen ink.
 */
export function normalizeWeather(rows: HourlyRow[], now: Date): WeatherValue {
  const windowEnd = now.getTime() + 24 * HOUR_MS;
  const byTime = new Map<number, HourlyRow>();
  for (const row of rows) {
    const time = row.at.getTime();
    if (time < now.getTime() || time >= windowEnd) continue;
    if (!Number.isFinite(row.temperature)) continue;
    byTime.set(time, row);
  }

  const entries = [...byTime.values()].sort(
    (a, b) => a.at.getTime() - b.at.getTime(),
  );

  const first = entries[0];
  const last = entries[entries.length - 1];
  if (entries.length < 23 || !first || !last) {
    throw new SourceError("Incomplete next-24-hour hourly forecast");
  }
  if (first.at.getTime() - now.getTime() > HOUR_MS) {
    throw new SourceError("Hourly forecast does not start within the hour");
  }
  if (last.at.getTime() - now.getTime() < 23 * HOUR_MS) {
    throw new SourceError("Hourly forecast does not reach 23 hours out");
  }
  for (let i = 1; i < entries.length; i += 1) {
    const previous = entries[i - 1] as HourlyRow;
    const current = entries[i] as HourlyRow;
    if (current.at.getTime() - previous.at.getTime() > HOUR_MS) {
      throw new SourceError("Hourly forecast has a gap wider than an hour");
    }
  }

  const slots = [0, 6, 12, 18].map((offset) => {
    const desired = now.getTime() + offset * HOUR_MS;
    const nearest = entries.reduce((best, entry) =>
      Math.abs(entry.at.getTime() - desired) <
      Math.abs(best.at.getTime() - desired)
        ? entry
        : best,
    );
    return {
      time: formatHourLabel(nearest.at),
      temp: Math.round(nearest.temperature),
      condition: nearest.condition,
    };
  });

  const temperatures = entries.map((entry) => entry.temperature);

  return {
    slots: [slots[0], slots[1], slots[2], slots[3]] as WeatherValue["slots"],
    low: Math.round(Math.min(...temperatures)),
    high: Math.round(Math.max(...temperatures)),
    hours: entries.length,
    unit: "°C",
    locationLabel: `${weatherLocation()?.label ?? "unknown location"} · Open-Meteo`,
    locationWarning: false,
    condition: slots[0]?.condition ?? "unknown",
  };
}

export function parseOpenMeteo(payload: unknown): HourlyRow[] {
  const parsed = OpenMeteoResponse.safeParse(payload);
  if (!parsed.success) throw new SourceError("Unexpected forecast shape");

  const { time, temperature_2m: temps, weather_code: codes } = parsed.data.hourly;
  if (time.length !== temps.length || time.length !== codes.length) {
    throw new SourceError("Forecast arrays are not the same length");
  }

  const rows: HourlyRow[] = [];
  for (let i = 0; i < time.length; i += 1) {
    const temperature = temps[i];
    const code = codes[i];
    const stamp = time[i];
    if (stamp === undefined || temperature === null || temperature === undefined) {
      continue;
    }
    // The API is asked for timezone=UTC and returns naive local stamps.
    const at = new Date(`${stamp}${stamp.endsWith("Z") ? "" : "Z"}`);
    if (Number.isNaN(at.getTime())) continue;
    rows.push({
      at,
      temperature,
      condition: conditionForWmoCode(code ?? -1),
    });
  }
  return rows;
}

function conditionForEccc(text: string): string {
  const value = text.toLowerCase();
  if (value.includes("thunder")) return "lightning-rainy";
  if (value.includes("snow") || value.includes("flurr")) return "snowy";
  if (value.includes("rain") || value.includes("shower") || value.includes("drizzle")) {
    return "rainy";
  }
  if (value.includes("fog") || value.includes("mist")) return "fog";
  if (value.includes("cloud") || value.includes("overcast")) return "cloudy";
  if (value.includes("sun") || value.includes("clear")) return "sunny";
  return "unknown";
}

export function parseEccc(payload: unknown): HourlyRow[] {
  const parsed = EcccResponse.safeParse(payload);
  if (!parsed.success) throw new SourceError("Unexpected Environment Canada forecast shape");
  const rows: HourlyRow[] = [];
  for (const item of parsed.data.properties.hourlyForecastGroup.hourlyForecasts) {
    const temperature = item.temperature.value.en;
    const at = new Date(item.timestamp);
    if (temperature === null || !Number.isFinite(temperature) || Number.isNaN(at.getTime())) {
      continue;
    }
    rows.push({
      at,
      temperature,
      condition: conditionForEccc(item.condition?.en ?? ""),
    });
  }
  return rows;
}

const SUCCESS_TTL_MS = 5 * 60_000;
const STALE_MAX_AGE_MS = 3 * 60 * 60_000;
const FAILURE_COOLDOWN_MS = 60_000;

let cachedForecast: { data: ModuleData<WeatherValue>; fetchedAt: number } | null = null;
let requestInFlight: Promise<ModuleData<WeatherValue>> | null = null;
let retryAfter = 0;
let lastFailure = "Forecast unavailable";

function cachedResult(nowMs: number, allowStale: boolean): ModuleData<WeatherValue> | null {
  if (!cachedForecast?.data.value) return null;
  const age = nowMs - cachedForecast.fetchedAt;
  if (age < 0 || age > (allowStale ? STALE_MAX_AGE_MS : SUCCESS_TTL_MS)) return null;
  if (!allowStale) return cachedForecast.data;
  return {
    ...cachedForecast.data,
    state: "stale",
    detail: `${lastFailure}; showing the last successful forecast`,
  };
}

/** Reset module state between unit tests. Never used by production code. */
export function resetOpenMeteoCacheForTests(): void {
  cachedForecast = null;
  requestInFlight = null;
  retryAfter = 0;
  lastFailure = "Forecast unavailable";
}

/**
 * Read a forecast without letting concurrent page/API renders hammer Open-Meteo.
 * A successful value is shared briefly. During a rate limit or transient outage,
 * the last good value remains visibly stale instead of disappearing from frozen
 * ink, and a short cooldown prevents every component from retrying at once.
 */
export async function readOpenMeteo(
  now: Date = new Date(),
): Promise<ModuleData<WeatherValue>> {
  const nowMs = now.getTime();
  const fresh = cachedResult(nowMs, false);
  if (fresh) return fresh;

  if (nowMs < retryAfter) {
    return (
      cachedResult(nowMs, true) ?? {
        state: "unavailable",
        detail: lastFailure,
      }
    );
  }

  if (requestInFlight) return requestInFlight;

  const primaryUrl = openMeteoUrl();
  if (primaryUrl === null) {
    return {
      state: "unavailable",
      detail: notConfigured(
        "Set NOTE4C_WEATHER_LATITUDE and NOTE4C_WEATHER_LONGITUDE to the place this panel should report on.",
      ),
    };
  }

  requestInFlight = (async () => {
    let primaryFailure = "Forecast unavailable";
    try {
      const payload = await fetchJson(primaryUrl, {
        timeoutMs: 20_000,
        headers: { "user-agent": TOWER_USER_AGENT },
      });
      const value = normalizeWeather(parseOpenMeteo(payload), now);
      const days = parseOpenMeteoDaily(payload);
      const data: ModuleData<WeatherValue> = {
        state: "ok",
        value: days.length > 0 ? { ...value, days } : value,
        observedAt: now.toISOString(),
      };
      cachedForecast = { data, fetchedAt: nowMs };
      retryAfter = 0;
      requestInFlight = null;
      return data;
    } catch (error) {
      primaryFailure = error instanceof SourceError ? error.message : "Forecast unavailable";
    }

    const fallbackUrl = fallbackForecastUrl();
    if (fallbackUrl.length === 0) {
      lastFailure = primaryFailure;
      retryAfter = nowMs + FAILURE_COOLDOWN_MS;
      requestInFlight = null;
      return (
        cachedResult(nowMs, true) ?? { state: "unavailable", detail: lastFailure }
      );
    }

    try {
      const payload = await fetchJson(fallbackUrl, {
        timeoutMs: 20_000,
        headers: { "user-agent": TOWER_USER_AGENT },
      });
      const normalized = normalizeWeather(parseEccc(payload), now);
      const data: ModuleData<WeatherValue> = {
        state: "ok",
        value: {
          ...normalized,
          locationLabel: `${weatherLocation()?.label ?? "unknown location"} · fallback forecast`,
        },
        observedAt: now.toISOString(),
      };
      cachedForecast = { data, fetchedAt: nowMs };
      retryAfter = 0;
      return data;
    } catch (error) {
      const fallbackFailure =
        error instanceof SourceError ? error.message : "Fallback forecast unavailable";
      lastFailure = `${primaryFailure}; fallback: ${fallbackFailure}`;
      retryAfter = nowMs + FAILURE_COOLDOWN_MS;
      return (
        cachedResult(nowMs, true) ?? {
          state: "unavailable",
          detail: lastFailure,
        }
      );
    } finally {
      requestInFlight = null;
    }
  })();

  return requestInFlight;
}
