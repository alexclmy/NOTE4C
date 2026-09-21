import { z } from "zod";
import { BLACK, RED, WHITE, YELLOW } from "@/core/palette";
import { DEFAULT_EXPRESSION } from "@/core/theme";
import {
  fillBandDither,
  fillDiscDither,
  scrubIsolatedAccents,
  type DitherStyle,
} from "../dither";
import {
  atlasFor,
  drawText,
  filled,
  reservedHeight,
  textElementSchema,
  textStyleSchema,
} from "../text";
import { ellipseInclusive } from "../draw";
import type { WeatherValue } from "../data";
import type { ModuleDefinition } from "../types";
import {
  clearModule,
  inner,
  provenanceShape,
  renderProvenance,
  renderUnavailable,
  staleShape,
  unavailableShape,
} from "./common";

/**
 * Weather hero — the number you read from the doorway.
 *
 * A full-bleed risograph sky (the same dither engine Sky uses) with the current
 * temperature set very large over it, the condition above and the day's range
 * below. It is the first module built for the 48/64 px atlas tier: the point is
 * a temperature that carries a room, not a slot in a strip.
 *
 * COLOUR obeys the dashboard's Expression. The warm field is yellow; an
 * Expressive board turns the sun red. Every accent is laid through the dither
 * engine and the frame is scrubbed once at the end, so the 2 px accent law
 * holds — which is also why the type is ink, never a thin red glyph the panel
 * would drop.
 *
 * DATA. The big number is the +0 h slot (the reading for now) and the range is
 * the window's low/high; the condition is the current one. Nothing here is
 * invented — an unavailable source renders an explicit unavailable state.
 */
export const WeatherHeroOptions = z.object({
  /** A place name the owner chooses, off by default (the source never asserts one). */
  location: textElementSchema({
    text: "",
    maxLength: 40,
    visible: false,
    style: { family: "plexmono", size: 13, weight: "bold" },
  }).default({}),
  conditionLine: textElementSchema({
    text: "{condition}",
    maxLength: 40,
    style: { family: "poppins", size: 22, weight: "bold" },
  }).default({}),
  /** The hero. Large by design — this is the module's whole reason to exist. */
  tempStyle: textStyleSchema({
    family: "poppins",
    size: 64,
    weight: "bold",
  }).default({}),
  rangeLine: textElementSchema({
    text: "H {high}°   ·   L {low}°",
    maxLength: 40,
    style: { family: "plexmono", size: 15, weight: "bold" },
  }).default({}),
  ...staleShape("Prévision périmée, affichée telle quelle"),
  ...unavailableShape({
    title: "Météo indisponible",
    note: "Aucune valeur inventée",
  }),
  ...provenanceShape({ size: 11 }),
});
export type WeatherHeroOptions = z.infer<typeof WeatherHeroOptions>;

const CONDITION_LABELS: Record<string, string> = {
  sunny: "Sunny",
  "clear-night": "Clear",
  partlycloudy: "Partly cloudy",
  cloudy: "Cloudy",
  rainy: "Rain",
  pouring: "Heavy rain",
  lightning: "Storms",
  "lightning-rainy": "Storms",
  snowy: "Snow",
  "snowy-rainy": "Sleet",
  fog: "Fog",
  windy: "Windy",
  hail: "Hail",
  exceptional: "Extreme",
};

function humaniseCondition(code: string): string {
  const known = CONDITION_LABELS[code];
  if (known) return known;
  const spaced = code.replace(/[-_]+/g, " ").trim();
  return spaced ? spaced.charAt(0).toUpperCase() + spaced.slice(1) : code;
}

export const weatherHero: ModuleDefinition<WeatherHeroOptions, WeatherValue> = {
  type: "weatherHero",
  label: "Weather hero",
  description:
    "The current temperature, set very large over a risograph sky, with the condition and the day's high and low. Reads from across the room. Renders an explicit unavailable state rather than any placeholder number.",
  schema: WeatherHeroOptions,
  defaultOptions: WeatherHeroOptions.parse({}),
  defaultSpan: { w: 8, h: 4 },
  minSpan: { w: 5, h: 3 },
  maxSpan: { w: 8, h: 6 },
  sourceBinding: "weather",

  render(fb, rect, data, options, ctx) {
    clearModule(fb, rect);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};

    if (data.state === "unavailable" || !data.value) {
      renderUnavailable(fb, rect, options, reporter);
      return;
    }

    const weather = data.value;
    const expression = ctx.theme?.expression ?? DEFAULT_EXPRESSION;
    const bw = expression.colourUse === "blackwhite";
    const expressive = expression.colourUse === "expressive";

    // The hero sky is a poster, not a texture swatch: it always reads in the
    // newsprint HALFTONE brush — round dots that grow from their centres — the
    // way a risograph weather poster does, whatever brush the rest of the board
    // uses. Dot SIZE still follows the board's pixel texture.
    const skyStyle: DitherStyle = {
      brush: "halftone",
      texture: expression.pixelTexture,
    };

    // The graded sky, dense across the top and fading all the way to bare paper
    // well before the foot — the `to` runs negative on purpose so the ramp hits
    // zero coverage around two thirds down, leaving the big number sitting on
    // paper the way the poster does, not on a busy field.
    const fieldPigment = bw ? BLACK : YELLOW;
    fillBandDither(fb, rect, fieldPigment, {
      from: expressive ? 0.62 : 0.46,
      to: expressive ? -0.35 : -0.28,
      axis: "y",
      style: skyStyle,
      background: WHITE,
    });

    // The sun, top-right, a nearly solid disc with a thin ink rim — the poster's
    // flat sun, not a faint stipple. A hair of edge softness keeps the rim from
    // aliasing without turning the disc into a cloud.
    const sunR = Math.max(9, Math.min(Math.round(rect.h * 0.22), 30));
    const sunCx = rect.x + rect.w - sunR - Math.round(rect.w * 0.06);
    const sunCy = rect.y + sunR + Math.round(rect.h * 0.1);
    const sunPigment = bw ? BLACK : expressive ? RED : YELLOW;
    fillDiscDither(fb, sunCx, sunCy, sunR, sunPigment, expressive ? 1 : 0.8, skyStyle, {
      background: WHITE,
      edgeSoftness: expressive ? 0 : 0.2,
    });
    ellipseInclusive(
      fb,
      Math.round(sunCx - sunR),
      Math.round(sunCy - sunR),
      Math.round(sunCx + sunR),
      Math.round(sunCy + sunR),
      undefined,
      BLACK,
    );

    // Type, all ink, over the field.
    const box = inner(rect);
    let topY = box.y;
    topY = drawText(fb, { ...box, y: topY, h: box.h - (topY - box.y) }, options.location, BLACK, "location", report).nextY;
    if (weather.locationLabel && options.showProvenance) {
      topY = renderProvenance(
        fb,
        { ...box, y: topY, h: box.h - (topY - box.y) },
        options,
        weather.locationLabel,
        weather.locationWarning ? RED : BLACK,
        reporter,
      );
    }
    drawText(
      fb,
      { ...box, y: topY, h: box.h - (topY - box.y) },
      filled(options.conditionLine, { condition: humaniseCondition(weather.condition) }),
      BLACK,
      "conditionLine",
      report,
    );

    // The hero number and the range, anchored to the foot.
    const rangeH = reservedHeight({
      text: "H 00° · L 00°",
      visible: options.rangeLine.visible,
      style: options.rangeLine.style,
    });
    const tempText = `${weather.slots[0]?.temp ?? weather.high}°`;
    const tempAtlas = atlasFor(options.tempStyle);
    const tempH = tempAtlas.lineHeight;
    const tempY = Math.max(topY, box.y + box.h - rangeH - tempH);
    fb.drawText(tempAtlas, box.x, tempY, tempText, BLACK);

    if (options.rangeLine.visible) {
      drawText(
        fb,
        { x: box.x, y: tempY + tempH, w: box.w, h: rangeH },
        filled(options.rangeLine, {
          high: weather.high,
          low: weather.low,
          unit: weather.unit,
        }),
        BLACK,
        "rangeLine",
        report,
      );
    }

    if (data.state === "stale") {
      drawText(
        fb,
        { ...box, y: box.y, h: box.h },
        options.staleLabel,
        BLACK,
        "staleLabel",
        report,
      );
    }

    // The second half of the 2 px guarantee: any accent sliver an ink glyph or
    // the sun's rim cut from the field is removed here.
    scrubIsolatedAccents(fb, rect, WHITE);
  },
};
