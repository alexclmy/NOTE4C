import { z } from "zod";
import type { FrameBuffer } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW, type PaletteIndex } from "@/core/palette";
import { DEFAULT_EXPRESSION, type Expression } from "@/core/theme";
import {
  ditherRect,
  fillBandDither,
  fillDiscDither,
  scrubIsolatedAccents,
  type DitherStyle,
} from "../dither";
import {
  atlasFor,
  drawText,
  reservedHeight,
  textElementSchema,
  type TextStyle,
} from "../text";
import { ellipseInclusive } from "../draw";
import {
  formatClock,
  formatDuration,
  moonPhase,
  MOON_PHASE_LABEL,
  sunDay,
  sunPosition,
  dayLengthDeltaMinutes,
} from "../astronomy";
import { PANEL_LATITUDE, PANEL_LONGITUDE, PANEL_TIMEZONE } from "../time";
import { chromeNow, type ModuleDefinition, type PixelRect } from "../types";
import { ADVANCED_OPTION_TAG, LAYOUT_VARIANT_TAG } from "../types";
import { clearModule, inner } from "./common";
import { isKnownTimeZone } from "./countdown";

/**
 * Sky — the day's light, drawn.
 *
 * Sunrise, sunset, how long the day is and whether it is drawing out or in, the
 * sun's place on its arc right now, and the moon's phase — all of it computed
 * on the Mac from a latitude, a longitude and the panel clock. There is no new
 * network source and there could not be one: this is geometry, and the
 * astronomy lives in ../astronomy.ts as pure closed-form maths.
 *
 * The centrepiece is the arc: a path from where the sun rose to where it will
 * set, with the sun itself laid on it as a dithered warm disc at today's
 * position, over a graded sky. The whole surface obeys the dashboard's
 * Expression — a Black & white sky is a tonal ink wash, an Expressive one is a
 * warm field with a red sun.
 *
 * A NOTE ON TIME. The day's numbers — sunrise, sunset, length, delta, moon —
 * are read from the real clock, because a new day genuinely is a new panel and
 * is worth the refresh. The sun's live position along the arc is read through
 * `chromeNow`, so it freezes when the frame is being hashed rather than shown:
 * the sun creeping a few pixels an hour is not, on its own, worth spending a
 * panel refresh, the same judgement the timestamp chrome makes about the clock.
 */

export const SKY_VARIANTS = ["arc", "horizon", "duo"] as const;
export type SkyVariant = (typeof SKY_VARIANTS)[number];

export const SkyOptions = z.object({
  variant: z.enum(SKY_VARIANTS).default("arc").describe(LAYOUT_VARIANT_TAG),
  title: textElementSchema({
    text: "LE CIEL",
    maxLength: 40,
    style: { family: "plexmono", size: 13, weight: "bold" },
  }).default({}),
  latitude: z
    .number()
    .min(-90)
    .max(90)
    .default(PANEL_LATITUDE)
    .describe(ADVANCED_OPTION_TAG),
  longitude: z
    .number()
    .min(-180)
    .max(180)
    .default(PANEL_LONGITUDE)
    .describe(ADVANCED_OPTION_TAG),
  timeZone: z
    .string()
    .max(64)
    .refine(isKnownTimeZone, {
      message:
        "Not a timezone this machine knows. Use an IANA name such as Europe/Paris or UTC.",
    })
    .default(PANEL_TIMEZONE)
    .describe(ADVANCED_OPTION_TAG),
  /** The moon disc and its phase name. Off makes a pure sun panel. */
  showMoon: z.boolean().default(true).describe(ADVANCED_OPTION_TAG),
});
export type SkyOptions = z.infer<typeof SkyOptions>;

interface SkyPalette {
  warm: boolean;
  skyPigment: PaletteIndex;
  sunPigment: PaletteIndex;
  skyTop: number;
  skyBottom: number;
  sunCoverage: number;
}

function skyPalette(expression: Expression): SkyPalette {
  const warm = expression.colourUse !== "blackwhite";
  if (!warm) {
    return {
      warm,
      skyPigment: BLACK,
      sunPigment: BLACK,
      skyTop: 0.16,
      skyBottom: 0.05,
      sunCoverage: 0.85,
    };
  }
  const expressive = expression.colourUse === "expressive";
  return {
    warm,
    skyPigment: YELLOW,
    sunPigment: RED,
    skyTop: expressive ? 0.6 : 0.42,
    skyBottom: expressive ? 0.22 : 0.14,
    sunCoverage: expressive ? 0.86 : 0.7,
  };
}

function styleOf(expression: Expression): DitherStyle {
  return { brush: expression.brush, texture: expression.pixelTexture };
}

/** Draw a machine value (a time, a duration) — not editable text, so not a role. */
function label(
  fb: FrameBuffer,
  x: number,
  y: number,
  text: string,
  style: TextStyle,
  colour: PaletteIndex,
  align: "left" | "center" | "right" = "left",
): void {
  const atlas = atlasFor(style);
  const penX = align === "left" ? x : align === "right" ? x : x;
  fb.drawText(atlas, penX, y, text, colour, align === "left" ? {} : { align });
}

const timeStyle: TextStyle = {
  family: "plexmono",
  size: 13,
  weight: "bold",
  align: "left",
  lineSpacing: 0,
  colour: "inherit",
};
const bigStyle: TextStyle = {
  family: "poppins",
  size: 22,
  weight: "bold",
  align: "left",
  lineSpacing: 0,
  colour: "inherit",
};
const noteStyle: TextStyle = {
  family: "inter",
  size: 13,
  weight: "regular",
  align: "left",
  lineSpacing: 0,
  colour: "inherit",
};

/**
 * The moon, drawn as a phase diagram: a full ink outline, its illuminated
 * portion filled with a dense ink grain, its shadow left as paper. The lit-side
 * geometry is a limb-and-terminator test, so a crescent is a crescent and a
 * gibbous is a gibbous rather than a half-and-half token.
 */
function drawMoon(
  fb: FrameBuffer,
  cx: number,
  cy: number,
  radius: number,
  phase: number,
  illumination: number,
  waxing: boolean,
  style: DitherStyle,
): void {
  if (radius < 3) return;
  const m = Math.cos(phase * 2 * Math.PI);
  const gibbous = illumination > 0.5;
  const limbSign = waxing ? 1 : -1;
  const r2 = radius * radius;
  ditherRect(
    fb,
    {
      x: Math.floor(cx - radius),
      y: Math.floor(cy - radius),
      w: Math.ceil(radius * 2) + 1,
      h: Math.ceil(radius * 2) + 1,
    },
    {
      pigment: BLACK,
      tone: 0.82,
      brush: style.brush,
      texture: style.texture,
      inside: (px, py) => {
        const nx = (px - cx) / radius;
        const ny = (py - cy) / radius;
        if (nx * nx + ny * ny > 1) return false;
        if ((px - cx) * (px - cx) + (py - cy) * (py - cy) > r2) return false;
        const hw = Math.sqrt(Math.max(0, 1 - ny * ny));
        const termX = Math.abs(m) * hw;
        const onLitHalf = nx * limbSign >= 0;
        return gibbous
          ? onLitHalf || Math.abs(nx) <= termX
          : onLitHalf && Math.abs(nx) >= termX;
      },
    },
  );
  // Full outline so the dark limb is a circle, not an absence.
  ellipseInclusive(
    fb,
    Math.round(cx - radius),
    Math.round(cy - radius),
    Math.round(cx + radius),
    Math.round(cy + radius),
    undefined,
    BLACK,
  );
}

/** Draw the arc and the sun/marker on it. Returns the horizon Y it drew to. */
function drawArc(
  fb: FrameBuffer,
  skyRect: PixelRect,
  arcMargin: number,
  sunArc: number,
  sunUp: boolean,
  pal: SkyPalette,
  style: DitherStyle,
): void {
  const left = skyRect.x + arcMargin;
  const right = skyRect.x + skyRect.w - arcMargin;
  const width = Math.max(1, right - left);
  const horizonY = skyRect.y + skyRect.h - 1;
  const arcHeight = Math.max(6, skyRect.h * 0.82);

  const pointAt = (t: number): { x: number; y: number } => ({
    x: left + t * width,
    y: horizonY - Math.sin(t * Math.PI) * arcHeight,
  });

  // A dotted arc: sample densely, ink every other short run, so it reads as a
  // drawn path rather than a hard rule.
  const steps = Math.max(24, Math.floor(width / 3));
  for (let i = 0; i <= steps; i += 1) {
    if (i % 3 === 2) continue; // the gaps in the dotted line
    const p = pointAt(i / steps);
    fb.set(Math.round(p.x), Math.round(p.y), BLACK);
  }

  // The sun, on the arc at today's fraction.
  const sun = pointAt(sunArc);
  const sunR = Math.max(5, Math.min(skyRect.h * 0.2, 15));
  if (sunUp) {
    fillDiscDither(fb, sun.x, sun.y, sunR, pal.sunPigment, pal.sunCoverage, style, {
      edgeSoftness: 0.35,
    });
    // A thin ink ring so the disc has an edge against a busy sky.
    ellipseInclusive(
      fb,
      Math.round(sun.x - sunR),
      Math.round(sun.y - sunR),
      Math.round(sun.x + sunR),
      Math.round(sun.y + sunR),
      undefined,
      BLACK,
    );
  } else {
    // Below the horizon: an open marker at the end it is nearest, so the panel
    // says "the sun is down" rather than dropping the sun with no explanation.
    const end = sunArc <= 0.5 ? pointAt(0) : pointAt(1);
    ellipseInclusive(
      fb,
      Math.round(end.x - 5),
      Math.round(end.y - 5),
      Math.round(end.x + 5),
      Math.round(end.y + 5),
      undefined,
      BLACK,
    );
  }
}

export const sky: ModuleDefinition<SkyOptions, never> = {
  type: "sky",
  label: "Sky",
  description:
    "Sunrise, sunset, day length and its day-to-day change, the sun on its arc, and the moon's phase — all computed on the Mac from your latitude and longitude. No network source.",
  schema: SkyOptions,
  defaultOptions: SkyOptions.parse({}),
  defaultSpan: { w: 5, h: 3 },
  minSpan: { w: 3, h: 2 },
  maxSpan: { w: 8, h: 4 },
  sourceBinding: "none",

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const expression = ctx.theme?.expression ?? DEFAULT_EXPRESSION;
    const style = styleOf(expression);
    const pal = skyPalette(expression);
    const reporter = ctx.report;
    const report = reporter ? { reporter } : {};
    const box = inner(rect);

    const day = sunDay(ctx.now, options.latitude, options.longitude);
    const delta = dayLengthDeltaMinutes(ctx.now, options.latitude, options.longitude);
    const marker = sunPosition(chromeNow(ctx), day);
    const moon = moonPhase(ctx.now);

    const sunriseText =
      day.kind === "normal" && day.sunrise
        ? formatClock(day.sunrise, options.timeZone)
        : day.kind === "polar-day"
          ? "—"
          : "—";
    const sunsetText =
      day.kind === "normal" && day.sunset
        ? formatClock(day.sunset, options.timeZone)
        : "—";
    const lengthText =
      day.kind === "polar-day"
        ? "Jour polaire"
        : day.kind === "polar-night"
          ? "Nuit polaire"
          : formatDuration(day.dayLengthMinutes);
    const deltaText =
      day.kind === "normal"
        ? `${delta >= 0 ? "+" : "-"}${Math.abs(delta)} min`
        : "";

    // Title.
    let headerY = box.y;
    if (reservedHeight(options.title) > 0) {
      headerY = drawText(fb, box, options.title, BLACK, "title", report).nextY + 1;
    }

    const args: RenderArgs = {
      pal,
      style,
      options,
      marker,
      moon,
      sunriseText,
      sunsetText,
      lengthText,
      deltaText,
    };

    if (options.variant === "duo") {
      renderDuo(fb, rect, box, headerY, args);
    } else if (options.variant === "horizon") {
      renderHorizon(fb, rect, box, headerY, args);
    } else {
      // --- arc (default) ---------------------------------------------
      // The footer is a times row and then a large day-length line. Reserve
      // both up front and size the sky to what is left, so the big number
      // always lands inside the tile instead of over its bottom edge.
      const timeH = reservedHeight({ text: "0", visible: true, style: timeStyle });
      const bigH = reservedHeight({ text: "0", visible: true, style: bigStyle });
      const footerTotal = timeH + bigH + 2;
      const skyRect: PixelRect = {
        x: box.x,
        y: headerY,
        w: box.w,
        h: Math.max(12, box.y + box.h - headerY - footerTotal),
      };
      // Graded sky, densest up high.
      fillBandDither(fb, skyRect, pal.skyPigment, {
        from: pal.skyTop,
        to: pal.skyBottom,
        axis: "y",
        style,
      });
      // Horizon rule.
      fb.fillRect(skyRect.x, skyRect.y + skyRect.h - 1, skyRect.w, 1, BLACK);
      drawArc(fb, skyRect, 14, marker.arc, marker.up, pal, style);

      // Sunrise / sunset at the two horizon ends, just under the line.
      const labelY = skyRect.y + skyRect.h + 1;
      label(fb, skyRect.x, labelY, sunriseText, timeStyle, BLACK, "left");
      label(fb, skyRect.x + skyRect.w, labelY, sunsetText, timeStyle, BLACK, "right");
      if (deltaText) {
        label(fb, skyRect.x + skyRect.w / 2, labelY, deltaText, timeStyle, BLACK, "center");
      }

      // Day length, large, along the bottom of the tile.
      const bigY = box.y + box.h - bigH;
      label(fb, skyRect.x + skyRect.w / 2, bigY, lengthText, bigStyle, BLACK, "center");

      // Moon, tucked into the top corner of the sky.
      if (options.showMoon) {
        const r = Math.max(6, Math.min(14, skyRect.h * 0.2));
        drawMoon(
          fb,
          skyRect.x + skyRect.w - r - 3,
          skyRect.y + r + 3,
          r,
          moon.phase,
          moon.illumination,
          moon.waxing,
          style,
        );
      }
    }

    // Black ink drawn over a warm dithered shape — the sun's ring, the horizon,
    // a time set on the sky — can clip a legal 2 px accent cell down to a lone
    // pixel. One final scrub over the whole tile removes exactly those, so the
    // 2 px rule holds for the finished module and not only for each fill.
    scrubIsolatedAccents(fb, rect);
  },
};

interface RenderArgs {
  pal: SkyPalette;
  style: DitherStyle;
  options: SkyOptions;
  marker: ReturnType<typeof sunPosition>;
  moon: ReturnType<typeof moonPhase>;
  sunriseText: string;
  sunsetText: string;
  lengthText: string;
  deltaText: string;
}

/** Sun on the left, moon on the right, each with its own reading. */
function renderDuo(
  fb: FrameBuffer,
  rect: PixelRect,
  box: PixelRect,
  headerY: number,
  a: RenderArgs,
): void {
  const half = Math.floor(box.w / 2);
  const skyRect: PixelRect = {
    x: box.x,
    y: headerY,
    w: half - 6,
    h: Math.max(12, box.y + box.h - headerY - reservedHeight({ text: "0", visible: true, style: timeStyle }) - 2),
  };
  fillBandDither(fb, skyRect, a.pal.skyPigment, {
    from: a.pal.skyTop,
    to: a.pal.skyBottom,
    axis: "y",
    style: a.style,
  });
  fb.fillRect(skyRect.x, skyRect.y + skyRect.h - 1, skyRect.w, 1, BLACK);
  drawArc(fb, skyRect, 10, a.marker.arc, a.marker.up, a.pal, a.style);
  const ly = skyRect.y + skyRect.h + 1;
  label(fb, skyRect.x, ly, a.sunriseText, timeStyle, BLACK, "left");
  label(fb, skyRect.x + skyRect.w, ly, a.sunsetText, timeStyle, BLACK, "right");
  label(fb, skyRect.x, ly + reservedHeight({ text: "0", visible: true, style: timeStyle }), a.lengthText, noteStyle, BLACK, "left");

  // Moon side.
  const moonCX = box.x + half + (box.w - half) / 2;
  const moonCY = headerY + Math.min(38, (box.y + box.h - headerY) * 0.42);
  const r = Math.max(10, Math.min(26, (box.w - half) * 0.3));
  drawMoon(fb, moonCX, moonCY, r, a.moon.phase, a.moon.illumination, a.moon.waxing, a.style);
  label(
    fb,
    moonCX,
    moonCY + r + 6,
    MOON_PHASE_LABEL[a.moon.name],
    noteStyle,
    BLACK,
    "center",
  );
}

/** A shallow sky band up top, the readouts large on paper below. */
function renderHorizon(
  fb: FrameBuffer,
  rect: PixelRect,
  box: PixelRect,
  headerY: number,
  a: RenderArgs,
): void {
  const skyRect: PixelRect = {
    x: box.x,
    y: headerY,
    w: box.w,
    h: Math.max(10, Math.floor((box.y + box.h - headerY) * 0.5)),
  };
  fillBandDither(fb, skyRect, a.pal.skyPigment, {
    from: a.pal.skyTop,
    to: a.pal.skyBottom,
    axis: "y",
    style: a.style,
  });
  fb.fillRect(skyRect.x, skyRect.y + skyRect.h - 1, skyRect.w, 1, BLACK);
  drawArc(fb, skyRect, 12, a.marker.arc, a.marker.up, a.pal, a.style);

  // Readouts on paper below.
  let y = skyRect.y + skyRect.h + 3;
  label(fb, box.x, y + reservedHeight({ text: "0", visible: true, style: bigStyle }) - 4, a.lengthText, bigStyle, BLACK, "left");
  if (a.deltaText) {
    label(fb, box.x + box.w, y + 2, a.deltaText, noteStyle, BLACK, "right");
  }
  y += reservedHeight({ text: "0", visible: true, style: bigStyle }) + 2;
  label(fb, box.x, y + reservedHeight({ text: "0", visible: true, style: timeStyle }) - 3, `${a.sunriseText} → ${a.sunsetText}`, timeStyle, BLACK, "left");
  if (a.options.showMoon) {
    const r = 11;
    drawMoon(fb, box.x + box.w - r - 2, y + r, r, a.moon.phase, a.moon.illumination, a.moon.waxing, a.style);
  }
}
