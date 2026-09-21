import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, WHITE, YELLOW } from "@/core/palette";
import { moduleDefinition } from "@/core/render/modules";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import { countIsolatedAccents } from "@/core/render/dither";
import type { DayForecast, WeatherValue } from "@/core/render/data";

const NOW = new Date("2026-09-21T12:00:00Z");

const DAYS: DayForecast[] = [
  { label: "MON", condition: "fog", high: 15, low: 10 },
  { label: "TUE", condition: "cloudy", high: 15, low: 4 },
  { label: "WED", condition: "sunny", high: 19, low: 7 },
  { label: "THU", condition: "sunny", high: 20, low: 8 },
  { label: "FRI", condition: "rainy", high: 17, low: 9 },
  { label: "SAT", condition: "partlycloudy", high: 23, low: 12 },
  { label: "SUN", condition: "sunny", high: 24, low: 13 },
];

const WEATHER: WeatherValue = {
  slots: [
    { time: "13h", temp: 18, condition: "partlycloudy" },
    { time: "19h", temp: 14, condition: "cloudy" },
    { time: "01h", temp: 10, condition: "clear-night" },
    { time: "07h", temp: 12, condition: "sunny" },
  ],
  low: 4,
  high: 24,
  hours: 24,
  unit: "°C",
  locationLabel: "Sample City",
  locationWarning: false,
  condition: "partlycloudy",
  days: DAYS,
};

function render(data: ModuleData<WeatherValue>): FrameBuffer {
  const definition = moduleDefinition("weatherWeek");
  const fb = new FrameBuffer(WHITE);
  definition.render(
    fb,
    cellsToPixels(0, 0, 8, 2),
    data,
    definition.schema.parse({}),
    { now: NOW, timeZone: "America/Toronto" },
  );
  return fb;
}

describe("weatherWeek module", () => {
  it("draws the seven-day outlook, packs cleanly, leaves no isolated accent", () => {
    const fb = render({ state: "ok", value: WEATHER });
    expect(() => pack(fb)).not.toThrow();
    expect(countIsolatedAccents(fb)).toBe(0);
    // Ink for the labels and temperatures.
    let black = 0;
    let yellow = 0;
    for (const p of fb.pixels) {
      if (p === BLACK) black += 1;
      if (p === YELLOW) yellow += 1;
    }
    expect(black).toBeGreaterThan(0);
    // The sunny days put a yellow sun on the panel.
    expect(yellow).toBeGreaterThan(0);
  });

  it("draws a hairline rule between each of the seven columns", () => {
    const fb = render({ state: "ok", value: WEATHER });
    // Six interior seams on an 8-cell-wide strip: sample a mid-height row and
    // count runs of black that are the column rules. The strip is 100 px tall
    // (2 cells), so mid-height is around y = 50.
    const y = 50;
    let seams = 0;
    for (let x = 2; x < 398; x += 1) {
      if (fb.get(x, y) === BLACK && fb.get(x - 1, y) !== BLACK) seams += 1;
    }
    // At least the six dividers (icons/text may add a few more black runs).
    expect(seams).toBeGreaterThanOrEqual(6);
  });

  it("renders an explicit unavailable state when the source has no daily block", () => {
    const noDays: WeatherValue = { ...WEATHER, days: undefined };
    const fb = render({ state: "ok", value: noDays });
    expect(() => pack(fb)).not.toThrow();
    expect(countIsolatedAccents(fb)).toBe(0);
    // Nothing from the strip: no yellow sun, because no day was drawn.
    let yellow = 0;
    for (const p of fb.pixels) if (p === YELLOW) yellow += 1;
    expect(yellow).toBe(0);
  });

  it("also renders unavailable when the whole source is unavailable", () => {
    const fb = render({ state: "unavailable" });
    expect(() => pack(fb)).not.toThrow();
    expect(countIsolatedAccents(fb)).toBe(0);
  });
});
