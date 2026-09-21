import { z } from "zod";
import { BLACK, RED } from "@/core/palette";
import { WEATHER_ICON_H, WEATHER_ICON_W, drawWeatherIcon } from "../sprites";
import {
  drawText,
  reservedHeight,
  textElementSchema,
  textStyleSchema,
} from "../text";
import type { WeatherValue } from "../data";
import type { ModuleDefinition } from "../types";
import {
  clearModule,
  inner,
  renderUnavailable,
  staleShape,
  unavailableShape,
} from "./common";

/**
 * Weather, seven days.
 *
 * A row of day columns — weekday, condition icon, high and low — ruled apart by
 * a hairline between each, the way a newspaper sets its outlook. It reads the
 * DAILY block the forecast source provides; the hourly-only fallback has none,
 * so this module renders an explicit unavailable state rather than a week of
 * invented numbers. That is the whole contract: no zero ever stands in for a
 * reading the tower did not get.
 *
 * The high is the number you read first, so it is the bold one; the low sits
 * under it, smaller. Times and temperatures are monospace, so every column's
 * digits line up whatever the weather does.
 */
export const WeatherWeekOptions = z.object({
  dayStyle: textStyleSchema({ family: "plexmono", size: 13, weight: "bold" }).default({}),
  highStyle: textStyleSchema({ family: "plexmono", size: 18, weight: "bold" }).default({}),
  lowStyle: textStyleSchema({ family: "plexmono", size: 13 }).default({}),
  /** The hairline rules between the day columns. On by design — the strip's structure. */
  dividers: z.boolean().default(true),
  heading: textElementSchema({
    text: "",
    maxLength: 40,
    visible: false,
    style: { size: 13, weight: "bold" },
  }).default({}),
  ...staleShape("Prévisions périmées, affichées telles quelles"),
  ...unavailableShape({
    title: "Prévisions 7 j indisponibles",
    note: "Aucune valeur inventée",
  }),
});
export type WeatherWeekOptions = z.infer<typeof WeatherWeekOptions>;

const MAX_DAYS = 7;

export const weatherWeek: ModuleDefinition<WeatherWeekOptions, WeatherValue> = {
  type: "weatherWeek",
  label: "Weather, 7 days",
  description:
    "A seven-day outlook: weekday, condition, high and low per column, ruled apart. Reads the daily forecast; renders an explicit unavailable state rather than any placeholder number when the source has no multi-day data.",
  schema: WeatherWeekOptions,
  defaultOptions: WeatherWeekOptions.parse({}),
  defaultSpan: { w: 8, h: 2 },
  minSpan: { w: 6, h: 2 },
  maxSpan: { w: 8, h: 3 },
  sourceBinding: "weather",

  render(fb, rect, data, options, ctx) {
    clearModule(fb, rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    const days = data.value?.days ?? [];
    if (data.state === "unavailable" || !data.value || days.length === 0) {
      // A weather source that is up but only hourly (the fallback) is still an
      // absent seven-day outlook as far as this module is concerned.
      renderUnavailable(fb, rect, options, reporter);
      return;
    }

    const box = inner(rect);
    let top = box.y;
    if (options.heading.visible) {
      top = drawText(fb, { ...box, y: top, h: box.h }, options.heading, BLACK, "heading", report).nextY;
    }
    if (data.state === "stale") {
      top = drawText(
        fb,
        { ...box, y: top, h: box.h },
        options.staleLabel,
        RED,
        "staleLabel",
        report,
      ).nextY;
    }

    const shown = days.slice(0, MAX_DAYS);
    const n = shown.length;
    const colWidth = box.w / n;

    const dayH = reservedHeight({ text: "MON", visible: true, style: options.dayStyle });
    const iconY = top + dayH;
    const highY = iconY + WEATHER_ICON_H + 1;
    const highH = reservedHeight({ text: "00°", visible: true, style: options.highStyle });
    const lowY = highY + highH;
    const lowH = Math.max(0, box.y + box.h - lowY);

    shown.forEach((day, index) => {
      const colLeft = box.x + Math.round(index * colWidth);
      const colRight = box.x + Math.round((index + 1) * colWidth);
      const colW = colRight - colLeft;
      const cell = { x: colLeft, w: colW };
      drawText(
        fb,
        { ...cell, y: top, h: dayH },
        { text: day.label, visible: true, style: { ...options.dayStyle, align: "center" } },
        BLACK,
        "day",
        report,
      );
      // The 28 px icon, centred in the column.
      drawWeatherIcon(fb, colLeft + Math.round((colW - WEATHER_ICON_W) / 2), iconY, day.condition);
      drawText(
        fb,
        { ...cell, y: highY, h: highH },
        { text: `${day.high}°`, visible: true, style: { ...options.highStyle, align: "center" } },
        BLACK,
        "high",
        report,
      );
      if (lowH > 0) {
        drawText(
          fb,
          { ...cell, y: lowY, h: lowH },
          { text: `${day.low}°`, visible: true, style: { ...options.lowStyle, align: "center" } },
          BLACK,
          "low",
          report,
        );
      }
    });

    // The rules between the columns, drawn last so they sit crisp over the
    // column edges. A hairline from just inside the top to just inside the foot.
    if (options.dividers && n > 1) {
      const y0 = rect.y + 2;
      const h = Math.max(1, rect.h - 4);
      for (let index = 1; index < n; index += 1) {
        const x = box.x + Math.round(index * colWidth);
        fb.fillRect(x, y0, 1, h, BLACK);
      }
    }
  },
};
