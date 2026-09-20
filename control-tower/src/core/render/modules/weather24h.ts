import { z } from "zod";
import { BLACK, RED } from "@/core/palette";
import { WEATHER_ICON_H, drawWeatherIcon } from "../sprites";
import {
  drawText,
  filled,
  reservedHeight,
  textElementSchema,
  textStyleSchema,
} from "../text";
import type { WeatherValue } from "../data";
import type { ModuleDefinition } from "../types";
import {
  PAD,
  clearModule,
  inner,
  provenanceShape,
  renderProvenance,
  renderUnavailable,
  staleShape,
  unavailableShape,
} from "./common";

/**
 * Weather, next 24 hours.
 *
 * This module used to force the forecast source's own label onto the panel —
 * "<place> · Open-Meteo" — under an option called `showLocation` that
 * defaulted to true. It was never asked for and it was rejected on the
 * physical device. It is now `showProvenance`, default false, and the separate
 * `location` role is where a place name the owner actually wants goes.
 *
 * The hourly times and temperatures are set in IBM Plex Mono by default. Every
 * digit in a monospace face is the same width, so the four columns stay
 * aligned as the numbers change and a 9 does not shift the column a 1 left.
 */
export const Weather24hOptions = z.object({
  heading: textElementSchema({
    text: "LES PROCHAINES 24H",
    maxLength: 40,
    style: { size: 15, weight: "bold" },
  }).default({}),
  /** A place name the owner chooses, not one the weather source asserts. */
  location: textElementSchema({
    text: "",
    maxLength: 40,
    visible: false,
    style: { size: 13 },
  }).default({}),
  slotTimeStyle: textStyleSchema({ family: "plexmono", size: 13 }).default({}),
  /** The number read from across the room, so it starts large. */
  tempStyle: textStyleSchema({
    family: "plexmono",
    size: 22,
    weight: "bold",
  }).default({}),
  rangeLine: textElementSchema({
    text: "{low} à {high} {unit} · {hours} h disponibles",
    maxLength: 80,
    style: { size: 13 },
  }).default({}),
  ...staleShape("Prévision périmée, affichée telle quelle"),
  ...unavailableShape({
    title: "Météo indisponible",
    note: "Aucune valeur inventée",
  }),
  ...provenanceShape({ size: 11 }),
});
export type Weather24hOptions = z.infer<typeof Weather24hOptions>;

export const weather24h: ModuleDefinition<Weather24hOptions, WeatherValue> = {
  type: "weather24h",
  label: "Weather, next 24 hours",
  description:
    "Four hourly slots at +0, +6, +12 and +18 hours with the low and high across the window. Renders an explicit unavailable state rather than any placeholder number.",
  schema: Weather24hOptions,
  defaultOptions: Weather24hOptions.parse({}),
  defaultSpan: { w: 8, h: 3 },
  minSpan: { w: 4, h: 2 },
  maxSpan: { w: 8, h: 4 },
  sourceBinding: "weather",

  migrateOptions(legacy) {
    return {
      ...(typeof legacy.heading === "string"
        ? { heading: { text: legacy.heading } }
        : {}),
      ...(typeof legacy.showRange === "boolean"
        ? { rangeLine: { visible: legacy.showRange } }
        : {}),
      // legacy.showLocation is deliberately NOT carried forward. It is the
      // option that drew the line the owner asked to be rid of; see src/core/migrate.ts.
      showProvenance: false,
    };
  },

  render(fb, rect, data, options, ctx) {
    clearModule(fb, rect);
    const box = inner(rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    if (data.state === "unavailable" || !data.value) {
      renderUnavailable(fb, rect, options, reporter);
      return;
    }

    const weather = data.value;
    let y = box.y;
    const below = (): { x: number; y: number; w: number; h: number } => ({
      x: box.x,
      y,
      w: box.w,
      h: box.h - (y - box.y),
    });

    y = drawText(fb, below(), options.heading, BLACK, "heading", report).nextY;
    y = drawText(fb, below(), options.location, BLACK, "location", report).nextY;
    y = renderProvenance(
      fb,
      below(),
      options,
      weather.locationLabel,
      weather.locationWarning ? RED : BLACK,
      reporter,
    );

    if (data.state === "stale") {
      y = drawText(fb, below(), options.staleLabel, RED, "staleLabel", report)
        .nextY;
    }

    // Four slots share the width evenly, so the module reads the same at any
    // span the grid allows.
    //
    // Time, icon and temperature are stacked rather than set side by side.
    // Beside the 28 px icon a temperature has about 42 px, which is not enough
    // for "-10°" above 15 px type; stacked it has the whole slot, so the
    // number can be as large as the panel deserves and a two-digit negative
    // does not trip the overflow mark every day all winter.
    const slotWidth = Math.floor(box.w / weather.slots.length);
    const timeHeight = reservedHeight({
      text: "00h",
      visible: true,
      style: options.slotTimeStyle,
    });
    const iconY = y + timeHeight;
    const tempY = iconY + WEATHER_ICON_H + 2;
    const tempHeight = reservedHeight({
      text: "0°",
      visible: true,
      style: options.tempStyle,
    });

    weather.slots.forEach((slot, index) => {
      const x = box.x + index * slotWidth;
      const slotBox = { x, w: Math.max(0, slotWidth - PAD) };
      drawText(
        fb,
        { ...slotBox, y, h: timeHeight },
        { text: slot.time, visible: true, style: options.slotTimeStyle },
        BLACK,
        "slotTime",
        report,
      );
      drawWeatherIcon(fb, x, iconY, slot.condition);
      drawText(
        fb,
        { ...slotBox, y: tempY, h: Math.max(0, box.y + box.h - tempY) },
        { text: `${slot.temp}°`, visible: true, style: options.tempStyle },
        BLACK,
        "slotTemp",
        report,
      );
    });

    const rangeY = tempY + tempHeight;
    const rangeHeight = box.y + box.h - rangeY;
    if (rangeHeight > 0) {
      drawText(
        fb,
        { x: box.x, y: rangeY, w: box.w, h: rangeHeight },
        filled(options.rangeLine, {
          low: weather.low,
          high: weather.high,
          unit: weather.unit,
          hours: weather.hours,
        }),
        BLACK,
        "rangeLine",
        report,
      );
    }
  },
};
