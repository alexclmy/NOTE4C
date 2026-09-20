import { describe, expect, it } from "vitest";
import {
  dayLengthDeltaMinutes,
  dayLengthMinutes,
  formatClock,
  formatDuration,
  moonPhase,
  sunDay,
  sunPosition,
} from "@/core/render/astronomy";

// London, so the numbers can be checked against a published almanac.
const LONDON = { lat: 51.5074, lon: -0.1278 };

describe("sun times", () => {
  it("puts London's late-September sunrise and sunset within a few minutes of the almanac", () => {
    // 2026-09-19: almanac sunrise ~06:47 BST, sunset ~19:00 BST.
    const day = sunDay(new Date("2026-09-19T12:00:00Z"), LONDON.lat, LONDON.lon);
    expect(day.kind).toBe("normal");
    const rise = formatClock(day.sunrise as Date, "Europe/London");
    const set = formatClock(day.sunset as Date, "Europe/London");
    const [rh, rm] = rise.split(":").map(Number);
    const [sh, sm] = set.split(":").map(Number);
    const riseMin = (rh as number) * 60 + (rm as number);
    const setMin = (sh as number) * 60 + (sm as number);
    expect(Math.abs(riseMin - (6 * 60 + 47))).toBeLessThanOrEqual(8);
    expect(Math.abs(setMin - (19 * 60 + 0))).toBeLessThanOrEqual(8);
  });

  it("has a day length near twelve hours at the equinox", () => {
    const minutes = dayLengthMinutes(
      new Date("2026-09-22T12:00:00Z"),
      LONDON.lat,
      LONDON.lon,
    );
    expect(Math.abs(minutes - 720)).toBeLessThan(25);
  });

  it("reports the days drawing in after the autumn equinox", () => {
    const delta = dayLengthDeltaMinutes(
      new Date("2026-10-01T12:00:00Z"),
      LONDON.lat,
      LONDON.lon,
    );
    expect(delta).toBeLessThan(0);
  });

  it("reports the days drawing out in spring", () => {
    const delta = dayLengthDeltaMinutes(
      new Date("2026-03-10T12:00:00Z"),
      LONDON.lat,
      LONDON.lon,
    );
    expect(delta).toBeGreaterThan(0);
  });

  it("names the polar cases instead of returning a broken time", () => {
    const midnightSun = sunDay(new Date("2026-06-21T12:00:00Z"), 78.2, 15.6);
    expect(midnightSun.kind).toBe("polar-day");
    expect(midnightSun.dayLengthMinutes).toBe(1440);
    const polarNight = sunDay(new Date("2026-12-21T12:00:00Z"), 78.2, 15.6);
    expect(polarNight.kind).toBe("polar-night");
    expect(polarNight.dayLengthMinutes).toBe(0);
  });
});

describe("sun position on the arc", () => {
  it("is 0 before dawn, ~0.5 at solar noon and clamps to 1 after dusk", () => {
    const day = sunDay(new Date("2026-06-21T12:00:00Z"), LONDON.lat, LONDON.lon);
    const beforeDawn = sunPosition(new Date("2026-06-21T02:00:00Z"), day);
    expect(beforeDawn.up).toBe(false);
    expect(beforeDawn.arc).toBe(0);
    const noon = sunPosition(day.transit, day);
    expect(noon.up).toBe(true);
    expect(Math.abs(noon.arc - 0.5)).toBeLessThan(0.05);
    expect(noon.altitude).toBeGreaterThan(0.98);
    const afterDusk = sunPosition(new Date("2026-06-21T23:30:00Z"), day);
    expect(afterDusk.arc).toBe(1);
  });
});

describe("moon phase", () => {
  it("keeps illumination within [0,1] and bottoms out near a known new moon", () => {
    // 2025-03-29 was a new moon.
    const nm = moonPhase(new Date("2025-03-29T10:00:00Z"));
    expect(nm.illumination).toBeGreaterThanOrEqual(0);
    expect(nm.illumination).toBeLessThan(0.08);
    expect(nm.name).toBe("new");
  });

  it("peaks near a known full moon and calls it full", () => {
    // 2025-04-13 was a full moon.
    const fm = moonPhase(new Date("2025-04-13T00:00:00Z"));
    expect(fm.illumination).toBeGreaterThan(0.92);
    expect(fm.name).toBe("full");
  });

  it("waxes in the first half of the cycle and wanes in the second", () => {
    const waxing = moonPhase(new Date("2025-04-04T00:00:00Z"));
    expect(waxing.waxing).toBe(true);
    const waning = moonPhase(new Date("2025-04-20T00:00:00Z"));
    expect(waning.waxing).toBe(false);
  });
});

describe("formatting", () => {
  it("prints a 24-hour clock and a duration the panel's way", () => {
    expect(formatClock(new Date("2026-09-19T05:07:00Z"), "UTC")).toBe("05:07");
    expect(formatDuration(750)).toBe("12 h 30");
  });
});
