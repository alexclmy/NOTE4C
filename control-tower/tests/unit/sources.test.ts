import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  conditionForWmoCode,
  normalizeWeather,
  parseEccc,
  parseOpenMeteo,
  readOpenMeteo,
  resetOpenMeteoCacheForTests,
  type HourlyRow,
} from "@/server/sources/openMeteo";
import {
  EXPECTED_SOURCE,
  MAX_AGE_SECONDS,
  normalizeEvents,
  readCalendarSnapshot,
  validateSnapshot,
} from "@/server/sources/calendarSnapshot";
import {
  assertReadOnlyEntity,
  formatSensor,
  parseDotenv,
  readCredentials,
  readSensor,
} from "@/server/sources/haSensor";
import { readComposerFeed } from "@/server/sources/composerFeed";
import { SourceError, fetchJson } from "@/server/sources/http";
import { sourceBindings, sourceHealth } from "@/server/sources";
import { starterDashboard } from "@/core/model";

const NOW = new Date("2026-09-11T05:00:00.000Z");

let scratch: string;

beforeEach(() => {
  resetOpenMeteoCacheForTests();
  scratch = fs.mkdtempSync(path.join(os.tmpdir(), "note4c-sources-"));
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.unstubAllEnvs();
  fs.rmSync(scratch, { recursive: true, force: true });
});

function hourlyRows(
  count: number,
  options: { startOffsetMs?: number; skip?: number[]; stepMs?: number } = {},
): HourlyRow[] {
  const step = options.stepMs ?? 3_600_000;
  const rows: HourlyRow[] = [];
  for (let i = 0; i < count; i += 1) {
    if (options.skip?.includes(i)) continue;
    rows.push({
      at: new Date(NOW.getTime() + (options.startOffsetMs ?? 0) + i * step),
      temperature: 10 + (i % 12),
      condition: "cloudy",
    });
  }
  return rows;
}

function weatherPayload(now: Date = NOW): unknown {
  const time: string[] = [];
  const temperature_2m: number[] = [];
  const weather_code: number[] = [];
  for (let i = 0; i < 48; i += 1) {
    time.push(new Date(now.getTime() + i * 3_600_000).toISOString().slice(0, 16));
    temperature_2m.push(12 + (i % 4));
    weather_code.push(i % 2);
  }
  return { hourly: { time, temperature_2m, weather_code } };
}

describe("Open-Meteo WMO mapping", () => {
  it("maps codes exactly as the composer does", () => {
    expect(conditionForWmoCode(0)).toBe("sunny");
    expect(conditionForWmoCode(1)).toBe("partlycloudy");
    expect(conditionForWmoCode(2)).toBe("partlycloudy");
    expect(conditionForWmoCode(3)).toBe("cloudy");
    expect(conditionForWmoCode(45)).toBe("fog");
    expect(conditionForWmoCode(48)).toBe("fog");
    expect(conditionForWmoCode(75)).toBe("snowy");
    expect(conditionForWmoCode(86)).toBe("snowy");
    expect(conditionForWmoCode(95)).toBe("lightning-rainy");
    expect(conditionForWmoCode(63)).toBe("rainy");
    expect(conditionForWmoCode(82)).toBe("rainy");
    expect(conditionForWmoCode(7)).toBe("unknown");
    expect(conditionForWmoCode(-1)).toBe("unknown");
  });
});

describe("weather coverage validation", () => {
  it("accepts a complete 24 hour forecast", () => {
    const value = normalizeWeather(hourlyRows(24), NOW);
    expect(value.slots).toHaveLength(4);
    expect(value.hours).toBe(24);
    expect(value.unit).toBe("°C");
    expect(value.locationWarning).toBe(false);
    expect(value.low).toBeLessThanOrEqual(value.high);
  });

  it("picks slots at +0, +6, +12 and +18 hours", () => {
    const value = normalizeWeather(hourlyRows(24), NOW);
    // 05:00Z is 01:00 in America/Toronto, so the slots land on 01, 07, 13, 19.
    expect(value.slots.map((slot) => slot.time)).toEqual([
      "01h",
      "07h",
      "13h",
      "19h",
    ]);
  });

  it("rejects fewer than 23 entries", () => {
    expect(() => normalizeWeather(hourlyRows(20), NOW)).toThrow(SourceError);
  });

  it("rejects a forecast that does not start within the hour", () => {
    // Dense enough to clear the count and gap checks, but the first entry is
    // 90 minutes out, so the panel would open on a stale hour.
    expect(() =>
      normalizeWeather(
        hourlyRows(40, { startOffsetMs: 5_400_000, stepMs: 2_700_000 }),
        NOW,
      ),
    ).toThrow(/does not start within the hour/);
  });

  it("rejects a forecast that stops short of 23 hours out", () => {
    expect(() =>
      normalizeWeather(hourlyRows(60, { stepMs: 1_200_000 }), NOW),
    ).toThrow(/does not reach 23 hours out/);
  });

  it("rejects a gap wider than one hour", () => {
    expect(() => normalizeWeather(hourlyRows(24, { skip: [10] }), NOW)).toThrow(
      /gap wider than an hour/,
    );
  });

  it("ignores entries outside the next 24 hours", () => {
    const rows = [
      ...hourlyRows(24),
      { at: new Date(NOW.getTime() - 3_600_000), temperature: -50, condition: "snowy" },
      { at: new Date(NOW.getTime() + 40 * 3_600_000), temperature: 99, condition: "sunny" },
    ];
    const value = normalizeWeather(rows, NOW);
    expect(value.hours).toBe(24);
    expect(value.low).toBeGreaterThan(-50);
    expect(value.high).toBeLessThan(99);
  });

  it("drops non-finite temperatures rather than rendering them", () => {
    const rows = hourlyRows(24);
    (rows[5] as HourlyRow).temperature = Number.NaN;
    expect(() => normalizeWeather(rows, NOW)).toThrow(/gap wider than an hour/);
  });
});

describe("Open-Meteo payload parsing", () => {
  it("parses the documented response shape", () => {
    const time: string[] = [];
    const temperature_2m: number[] = [];
    const weather_code: number[] = [];
    for (let i = 0; i < 30; i += 1) {
      time.push(new Date(NOW.getTime() + i * 3_600_000).toISOString().slice(0, 16));
      temperature_2m.push(12 + i);
      weather_code.push(3);
    }
    const rows = parseOpenMeteo({ hourly: { time, temperature_2m, weather_code } });
    expect(rows).toHaveLength(30);
    expect(rows[0]?.condition).toBe("cloudy");
  });

  it("skips null temperatures and keeps the rest", () => {
    const rows = parseOpenMeteo({
      hourly: {
        time: ["2026-09-11T05:00", "2026-09-11T06:00"],
        temperature_2m: [null, 14],
        weather_code: [3, 0],
      },
    });
    expect(rows).toHaveLength(1);
    expect(rows[0]?.condition).toBe("sunny");
  });

  it("refuses a malformed payload", () => {
    expect(() => parseOpenMeteo({ hourly: { time: ["x"] } })).toThrow(SourceError);
    expect(() => parseOpenMeteo(null)).toThrow(SourceError);
  });

  it("refuses arrays of differing lengths", () => {
    expect(() =>
      parseOpenMeteo({
        hourly: { time: ["a", "b"], temperature_2m: [1], weather_code: [0] },
      }),
    ).toThrow(/same length/);
  });
});

describe("readOpenMeteo", () => {
  /**
   * The forecast is for wherever the operator said, and nowhere by default.
   * These tests configure a location explicitly — TEST-NET-ish coordinates in
   * the Atlantic, so a stray real request could not be mistaken for anybody's
   * home — and the unconfigured case is its own test.
   */
  beforeEach(() => {
    vi.stubEnv("NOTE4C_WEATHER_LATITUDE", "0");
    vi.stubEnv("NOTE4C_WEATHER_LONGITUDE", "0");
    vi.stubEnv("NOTE4C_WEATHER_LABEL", "Null Island");
    vi.stubEnv("NOTE4C_WEATHER_FALLBACK_URL", "");
  });

  it("is not configured, and fetches nothing, without coordinates", async () => {
    vi.stubEnv("NOTE4C_WEATHER_LATITUDE", "");
    vi.stubEnv("NOTE4C_WEATHER_LONGITUDE", "");
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);

    const result = await readOpenMeteo(NOW);
    expect(result.state).toBe("unavailable");
    expect(result.detail).toContain("Not configured");
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("names the configured location on the value it returns", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify(weatherPayload()), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      ),
    );
    const result = await readOpenMeteo(NOW);
    expect(result.value?.locationLabel).toBe("Null Island · Open-Meteo");
  });

  it("returns ok for a healthy forecast", async () => {
    const time: string[] = [];
    const temperature_2m: number[] = [];
    const weather_code: number[] = [];
    for (let i = 0; i < 48; i += 1) {
      time.push(new Date(NOW.getTime() + i * 3_600_000).toISOString().slice(0, 16));
      temperature_2m.push(12);
      weather_code.push(0);
    }
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify({ hourly: { time, temperature_2m, weather_code } }), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      ),
    );
    const result = await readOpenMeteo(NOW);
    expect(result.state).toBe("ok");
    expect(result.value?.hours).toBe(24);
  });

  it("shares one successful forecast across repeated readers", async () => {
    const fetchMock = vi.fn(async () =>
      new Response(JSON.stringify(weatherPayload()), {
        status: 200,
        headers: { "content-type": "application/json" },
      }),
    );
    vi.stubGlobal("fetch", fetchMock);

    const [first, second, third] = await Promise.all([
      readOpenMeteo(NOW),
      readOpenMeteo(NOW),
      readOpenMeteo(NOW),
    ]);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect([first.state, second.state, third.state]).toEqual(["ok", "ok", "ok"]);
  });

  it("falls back to the configured second source when the first is rate limited", async () => {
    vi.stubEnv("NOTE4C_WEATHER_FALLBACK_URL", "https://example.invalid/forecast");
    const ecccPayload = {
      properties: {
        hourlyForecastGroup: {
          hourlyForecasts: Array.from({ length: 24 }, (_, i) => ({
            timestamp: new Date(NOW.getTime() + i * 3_600_000).toISOString(),
            condition: { en: i % 2 ? "Cloudy" : "Mainly sunny" },
            temperature: { value: { en: 18 + i / 10 } },
          })),
        },
      },
    };
    const fetchMock = vi
      .fn()
      .mockResolvedValueOnce(new Response("limited", { status: 429 }))
      .mockResolvedValueOnce(
        new Response(JSON.stringify(ecccPayload), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      );
    vi.stubGlobal("fetch", fetchMock);

    expect(parseEccc(ecccPayload)).toHaveLength(24);
    const result = await readOpenMeteo(NOW);
    expect(result.state).toBe("ok");
    expect(result.value?.hours).toBe(24);
    expect(result.value?.locationLabel).toContain("fallback forecast");
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it("keeps the last successful forecast visibly stale during a rate limit", async () => {
    const fetchMock = vi
      .fn()
      .mockResolvedValueOnce(
        new Response(JSON.stringify(weatherPayload()), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      )
      .mockResolvedValue(new Response("limited", { status: 429 }));
    vi.stubGlobal("fetch", fetchMock);

    const healthy = await readOpenMeteo(NOW);
    const sixMinutesLater = new Date(NOW.getTime() + 6 * 60_000);
    const limited = await readOpenMeteo(sixMinutesLater);
    const cooldownRead = await readOpenMeteo(new Date(sixMinutesLater.getTime() + 10_000));

    expect(healthy.state).toBe("ok");
    expect(limited.state).toBe("stale");
    expect(limited.value).toEqual(healthy.value);
    expect(limited.detail).toContain("HTTP 429");
    expect(cooldownRead.state).toBe("stale");
    // Two: the healthy read and the rate-limited one. The third used to be an
    // attempt at the second source, which this test no longer configures.
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it("returns unavailable, never a placeholder, when the API fails", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => new Response("nope", { status: 503 })),
    );
    const result = await readOpenMeteo(NOW);
    expect(result.state).toBe("unavailable");
    expect(result.value).toBeUndefined();
    expect(result.detail).toContain("HTTP 503");
  });

  it("does not mention a fallback it was never given", async () => {
    // With no second source configured there is nothing to fall back to, and
    // a detail line that talks about one sends the reader looking for a
    // failure that did not happen.
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => new Response("nope", { status: 503 })),
    );
    const result = await readOpenMeteo(NOW);
    expect(result.detail).not.toContain("fallback");
  });

  it("aborts and reports a timeout rather than hanging a request", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(
        async (_url: string, init?: RequestInit) =>
          await new Promise<Response>((_resolve, reject) => {
            init?.signal?.addEventListener("abort", () => {
              reject(Object.assign(new Error("aborted"), { name: "AbortError" }));
            });
          }),
      ),
    );
    // Exercised through fetchJson directly so the assertion does not depend on
    // sitting through the adapter's real 20 second budget.
    await expect(fetchJson("https://example.invalid/x", { timeoutMs: 20 })).rejects.toThrow(
      /Timed out/,
    );
  });

  it("does not leak the request URL into the detail string", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => {
        throw new TypeError("fetch failed: https://api.open-meteo.com/secret");
      }),
    );
    const result = await readOpenMeteo(NOW);
    expect(result.detail).not.toMatch(/open-meteo\.com/);
  });
});

function snapshot(overrides: Record<string, unknown> = {}): unknown {
  return {
    schema: 1,
    source: EXPECTED_SOURCE,
    // Any calendar name parses; NOTE4C_CALENDAR_NAME asserts one when set.
    calendar: "Household",
    state: "ok",
    observed_at: new Date(NOW.getTime() - 30_000).toISOString(),
    tick: 12,
    worker_pid: 4321,
    events: [
      {
        title: "Team standup",
        start: new Date(NOW.getTime() + 7_200_000).toISOString(),
        end: new Date(NOW.getTime() + 10_800_000).toISOString(),
        all_day: false,
        local_date: "2026-09-11",
      },
    ],
    ...overrides,
  };
}

describe("calendar snapshot contract", () => {
  afterEach(() => {
    delete process.env.NOTE4C_CALENDAR_NAME;
  });

  it("accepts a fresh, correctly scoped snapshot", () => {
    expect(validateSnapshot(snapshot(), NOW).calendar).toBe("Household");
  });

  it("accepts any calendar name when none is configured", () => {
    // The name used to be a literal in the schema, which made one household's
    // calendar part of the file format.
    expect(validateSnapshot(snapshot({ calendar: "Anything" }), NOW).calendar).toBe(
      "Anything",
    );
  });

  it("insists on the exact spelling when one is configured", () => {
    process.env.NOTE4C_CALENDAR_NAME = "Household";
    expect(() => validateSnapshot(snapshot({ calendar: "Housewhold" }), NOW)).toThrow(
      SourceError,
    );
  });

  it("insists on the exact source string", () => {
    expect(() =>
      validateSnapshot(snapshot({ source: "EventKit" }), NOW),
    ).toThrow(SourceError);
  });

  it("insists on schema 1", () => {
    expect(() => validateSnapshot(snapshot({ schema: 2 }), NOW)).toThrow(
      SourceError,
    );
  });

  it("rejects a snapshot older than 180 seconds", () => {
    const old = snapshot({
      observed_at: new Date(NOW.getTime() - (MAX_AGE_SECONDS + 1) * 1000).toISOString(),
    });
    expect(() => validateSnapshot(old, NOW)).toThrow(/stale or future-dated/);
  });

  it("rejects a future-dated snapshot", () => {
    const future = snapshot({
      observed_at: new Date(NOW.getTime() + 5_000).toISOString(),
    });
    expect(() => validateSnapshot(future, NOW)).toThrow(/stale or future-dated/);
  });

  it("rejects unexpected event fields, so private data cannot arrive", () => {
    const leaky = snapshot({
      events: [
        {
          title: "Rendez-vous",
          start: new Date(NOW.getTime() + 3_600_000).toISOString(),
          end: new Date(NOW.getTime() + 7_200_000).toISOString(),
          all_day: false,
          local_date: "2026-09-11",
          location: "12 rue Privee",
        },
      ],
    });
    expect(() => validateSnapshot(leaky, NOW)).toThrow(SourceError);
  });

  it("rejects a worker that is not reporting ok", () => {
    expect(() => validateSnapshot(snapshot({ state: "denied" }), NOW)).toThrow(
      SourceError,
    );
  });
});

describe("calendar event normalisation", () => {
  it("drops events that have already ended", () => {
    const payload = validateSnapshot(
      snapshot({
        events: [
          {
            title: "Fini",
            start: new Date(NOW.getTime() - 7_200_000).toISOString(),
            end: new Date(NOW.getTime() - 3_600_000).toISOString(),
            all_day: false,
            local_date: "2026-09-11",
          },
        ],
      }),
      NOW,
    );
    expect(normalizeEvents(payload, NOW)).toEqual([]);
  });

  it("labels all-day events with the journée wording", () => {
    const payload = validateSnapshot(
      snapshot({
        events: [
          {
            title: "Livraison",
            start: new Date(NOW.getTime() + 3_600_000).toISOString(),
            end: new Date(NOW.getTime() + 90_000_000).toISOString(),
            all_day: true,
            local_date: "2026-09-12",
          },
        ],
      }),
      NOW,
    );
    expect(normalizeEvents(payload, NOW)[0]?.when).toBe("12/09 · journée");
  });

  it("sorts by start time and de-duplicates identical events", () => {
    const later = new Date(NOW.getTime() + 20_000_000).toISOString();
    const sooner = new Date(NOW.getTime() + 3_600_000).toISOString();
    const payload = validateSnapshot(
      snapshot({
        events: [
          { title: "B", start: later, end: later, all_day: false, local_date: "2026-09-11" },
          { title: "A", start: sooner, end: later, all_day: false, local_date: "2026-09-11" },
          { title: "A", start: sooner, end: later, all_day: false, local_date: "2026-09-11" },
        ],
      }),
      NOW,
    );
    const events = normalizeEvents(payload, NOW);
    expect(events.map((e) => e.title)).toEqual(["A", "B"]);
  });

  it("keeps only titles and times", () => {
    const payload = validateSnapshot(snapshot(), NOW);
    expect(Object.keys(normalizeEvents(payload, NOW)[0] ?? {}).sort()).toEqual([
      "end",
      "start",
      "title",
      "when",
    ]);
  });
});

describe("readCalendarSnapshot", () => {
  it("returns ok for a healthy snapshot file", () => {
    const file = path.join(scratch, "snapshot.json");
    fs.writeFileSync(file, JSON.stringify(snapshot()));
    const result = readCalendarSnapshot(NOW, file);
    expect(result.state).toBe("ok");
    expect(result.value?.events).toHaveLength(1);
  });

  it("returns unavailable when the file is absent", () => {
    const result = readCalendarSnapshot(NOW, path.join(scratch, "nope.json"));
    expect(result.state).toBe("unavailable");
    expect(result.detail).toMatch(/worker may be stopped/);
  });

  it("returns unavailable for malformed JSON", () => {
    const file = path.join(scratch, "snapshot.json");
    fs.writeFileSync(file, "{ not json");
    expect(readCalendarSnapshot(NOW, file).state).toBe("unavailable");
  });

  it("returns unavailable, not stale-with-value, for an old snapshot", () => {
    const file = path.join(scratch, "snapshot.json");
    fs.writeFileSync(
      file,
      JSON.stringify(
        snapshot({ observed_at: new Date(NOW.getTime() - 600_000).toISOString() }),
      ),
    );
    const result = readCalendarSnapshot(NOW, file);
    expect(result.state).toBe("unavailable");
    expect(result.value).toBeUndefined();
  });
});

describe("dotenv parsing", () => {
  it("reads plain, quoted and exported assignments", () => {
    const env = parseDotenv(
      [
        "# comment",
        "",
        "HASS_URL=http://homeassistant.local:8123",
        'HASS_TOKEN="abc123"',
        "export OTHER='xyz'",
        "MALFORMED",
        "=novalue",
      ].join("\n"),
    );
    expect(env.HASS_URL).toBe("http://homeassistant.local:8123");
    expect(env.HASS_TOKEN).toBe("abc123");
    expect(env.OTHER).toBe("xyz");
    expect(env.MALFORMED).toBeUndefined();
  });
});

describe("HA credentials", () => {
  it("reads the configured env file without copying it anywhere", () => {
    const envPath = path.join(scratch, ".env");
    fs.writeFileSync(envPath, "HASS_URL=http://ha.local:8123/\nHASS_TOKEN=tok\n");
    const credentials = readCredentials(envPath);
    expect(credentials.url).toBe("http://ha.local:8123");
    expect(credentials.token).toBe("tok");
  });

  it("throws when credentials are absent", () => {
    vi.stubEnv("HASS_URL", "");
    vi.stubEnv("HASS_TOKEN", "");
    // Not configured rather than unavailable: nothing failed, nobody has said
    // where the credentials live. The UI renders the two differently.
    expect(() => readCredentials(path.join(scratch, "missing"))).toThrow(
      /Not configured/,
    );
  });
});

describe("sensor entity allowlist", () => {
  it("accepts read-only domains", () => {
    expect(assertReadOnlyEntity("sensor.temperature_exterieure")).toBeTruthy();
    expect(assertReadOnlyEntity("binary_sensor.porte")).toBeTruthy();
  });

  it("refuses anything actuatable", () => {
    for (const entity of [
      "light.kitchen",
      "switch.pump",
      "lock.front_door",
      "script.evacuate",
      "sensor.Uppercase",
      "sensor.",
    ]) {
      expect(() => assertReadOnlyEntity(entity)).toThrow(SourceError);
    }
  });
});

describe("sensor formatting", () => {
  it("uses the composer's French words for non-numeric states", () => {
    expect(formatSensor({ entity_id: "sensor.a", state: "unknown" }, NOW).value).toBe(
      "inconnu",
    );
    expect(
      formatSensor({ entity_id: "sensor.a", state: "unavailable" }, NOW).value,
    ).toBe("indisponible");
    expect(formatSensor({ entity_id: "sensor.a", state: "21" }, NOW).value).toBe(
      "date inconnue",
    );
  });

  it("appends the unit", () => {
    const result = formatSensor(
      {
        entity_id: "sensor.a",
        state: "19.4",
        attributes: { unit_of_measurement: "°C" },
        last_reported: new Date(NOW.getTime() - 60_000).toISOString(),
      },
      NOW,
    );
    expect(result).toEqual({ value: "19.4 °C", stale: false });
  });

  it("marks a reading older than three hours as périmé", () => {
    const result = formatSensor(
      {
        entity_id: "sensor.a",
        state: "19.4",
        last_reported: new Date(NOW.getTime() - 4 * 3_600_000).toISOString(),
      },
      NOW,
    );
    expect(result).toEqual({ value: "périmé", stale: true });
  });

  it("treats exactly three hours as still fresh", () => {
    const result = formatSensor(
      {
        entity_id: "sensor.a",
        state: "19.4",
        last_reported: new Date(NOW.getTime() - 3 * 3_600_000).toISOString(),
      },
      NOW,
    );
    expect(result.stale).toBe(false);
  });
});

describe("readSensor", () => {
  it("returns a formatted reading", async () => {
    const envPath = path.join(scratch, ".env");
    fs.writeFileSync(envPath, "HASS_URL=http://ha.local:8123\nHASS_TOKEN=tok\n");
    const seen: string[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string, init?: RequestInit) => {
        seen.push(String(url));
        expect(
          new Headers(init?.headers).get("authorization"),
        ).toBe("Bearer tok");
        return new Response(
          JSON.stringify({
            entity_id: "sensor.temp",
            state: "19.4",
            attributes: { unit_of_measurement: "°C" },
            last_reported: new Date(NOW.getTime() - 60_000).toISOString(),
          }),
          { status: 200, headers: { "content-type": "application/json" } },
        );
      }),
    );

    const result = await readSensor("sensor.temp", "Capteur", NOW, { envPath });
    expect(result.state).toBe("ok");
    expect(result.value).toEqual({ label: "Capteur", value: "19.4 °C" });
    expect(seen[0]).toBe("http://ha.local:8123/api/states/sensor.temp");
  });

  it("refuses an actuatable entity before making any request", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    const result = await readSensor("light.kitchen", "Lumière", NOW);
    expect(result.state).toBe("unavailable");
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("never puts the token in the returned detail", async () => {
    const envPath = path.join(scratch, ".env");
    fs.writeFileSync(envPath, "HASS_URL=http://ha.local:8123\nHASS_TOKEN=supersecret\n");
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => new Response("no", { status: 401 })),
    );
    const result = await readSensor("sensor.temp", "Capteur", NOW, { envPath });
    expect(result.state).toBe("unavailable");
    expect(JSON.stringify(result)).not.toContain("supersecret");
  });
});

describe("composer feed", () => {
  // The composer is off unless an origin is configured, so every case here
  // sets one first. The unconfigured case is its own test below.
  beforeEach(() => {
    vi.stubEnv("NOTE4C_COMPOSER_ORIGIN", "http://127.0.0.1:9731");
  });

  it("is not configured, and says so, when no origin is set", async () => {
    vi.stubEnv("NOTE4C_COMPOSER_ORIGIN", "");
    const feed = await readComposerFeed(NOW);
    expect(feed.state).toBe("not_configured");
  });

  it("reports running when the composer answers", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(
          JSON.stringify({ checked_at: NOW.toISOString(), auto_push: false }),
          { status: 200, headers: { "content-type": "application/json" } },
        ),
      ),
    );
    const feed = await readComposerFeed(NOW);
    expect(feed.state).toBe("running");
  });

  it("treats absence as a normal state, not an error", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => {
        throw new TypeError("fetch failed");
      }),
    );
    const feed = await readComposerFeed(NOW);
    expect(feed.state).toBe("not_running");
  });

  it("reports an unexpected shape distinctly from absence", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify({ sources: "not an object" }), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      ),
    );
    expect((await readComposerFeed(NOW)).state).toBe("unreadable");
  });
});

describe("source bindings", () => {
  it("lists only what a dashboard actually uses", () => {
    const bindings = sourceBindings(starterDashboard("Kitchen panel"));
    expect(bindings.weather).toBe(true);
    expect(bindings.calendar).toBe(true);
    expect(bindings.sensors).toEqual([
      { entityId: "sensor.example_temperature", label: "Capteur" },
    ]);
  });

  it("reports nothing for an empty dashboard", () => {
    const doc = starterDashboard("x");
    doc.modules = [];
    expect(sourceBindings(doc)).toEqual({
      weather: false,
      calendar: false,
      sensors: [],
      remindersList: null,
    });
  });
});

describe("source health rows", () => {
  it("distinguishes not configured from unavailable", () => {
    const rows = sourceHealth({
      weather: { state: "ok", observedAt: NOW.toISOString() },
      calendar: { state: "unavailable", detail: "worker stopped" },
      sensors: {},
      reminders: { state: "unavailable", detail: "Not used by this dashboard" },
    });
    expect(rows.map((row) => `${row.key}:${row.state}`)).toEqual([
      "weather:ok",
      "calendar:unavailable",
      "reminders:not_configured",
      "sensors:not_configured",
    ]);
  });
});
