import { FrameBuffer, type Sprite } from "@/core/frame";
import { BLACK, RED, WHITE, YELLOW, type PaletteIndex } from "@/core/palette";
import { ellipseInclusive, line, polygon, polyline, rectInclusive } from "./draw";

/**
 * Ports of the composer's pixel art, transcribed from
 * note4c-dashboard/dashboard.py octopus() and weather_icon().
 *
 * Pixel art at native integer scale on purpose: no assets, no network, no
 * model calls in any render path, ever.
 */

const OCTOPUS_W = 29;
const OCTOPUS_H = 26;
export const OCTOPUS_SCALE = 2;
export const OCTOPUS_DRAWN_W = OCTOPUS_W * OCTOPUS_SCALE; // 58
export const OCTOPUS_DRAWN_H = OCTOPUS_H * OCTOPUS_SCALE; // 52

const OCTOPUS_BODY = [
  "0001111111000",
  "0011111111100",
  "0111111111110",
  "1111111111111",
  "1111111111111",
  "1111111111111",
  "1111111111111",
  "0111111111110",
  "0011111111100",
  "0110110110110",
  "1100110110011",
  "1001100011001",
] as const;

/**
 * A tiny FrameBuffer-shaped surface so the PIL-compatible draw helpers can be
 * reused for sprites without a second implementation.
 */
class Surface {
  readonly width: number;
  readonly height: number;
  readonly pixels: Uint8Array;

  constructor(width: number, height: number, fill: PaletteIndex) {
    this.width = width;
    this.height = height;
    this.pixels = new Uint8Array(width * height).fill(fill);
  }

  set(x: number, y: number, color: PaletteIndex): void {
    const px = x | 0;
    const py = y | 0;
    if (px < 0 || py < 0 || px >= this.width || py >= this.height) return;
    this.pixels[py * this.width + px] = color;
  }

  get(x: number, y: number): number {
    const px = x | 0;
    const py = y | 0;
    if (px < 0 || py < 0 || px >= this.width || py >= this.height) return -1;
    return this.pixels[py * this.width + px] as number;
  }

  fillRect(x: number, y: number, w: number, h: number, color: PaletteIndex): void {
    const x0 = Math.max(0, Math.trunc(x));
    const y0 = Math.max(0, Math.trunc(y));
    const x1 = Math.min(this.width, Math.trunc(x) + Math.trunc(w));
    const y1 = Math.min(this.height, Math.trunc(y) + Math.trunc(h));
    for (let py = y0; py < y1; py += 1) {
      this.pixels.fill(color, py * this.width + x0, py * this.width + x1);
    }
  }

  toSprite(): Sprite {
    return { w: this.width, h: this.height, data: this.pixels };
  }
}

/** The draw helpers only need set/fillRect, which Surface provides. */
function asDrawTarget(surface: Surface): FrameBuffer {
  return surface as unknown as FrameBuffer;
}

export interface OctopusOptions {
  condition: string;
  /** Current temperature in Celsius, when known. Drives the scarf. */
  temp?: number;
}

export function octopusSprite({ condition, temp }: OctopusOptions): Sprite {
  const surface = new Surface(OCTOPUS_W, OCTOPUS_H, WHITE);
  const d = asDrawTarget(surface);

  OCTOPUS_BODY.forEach((row, yy) => {
    [...row].forEach((value, xx) => {
      if (value === "1") surface.set(xx + 7, yy + 9, RED);
    });
  });

  const sleepy = condition === "clear-night";
  for (const xx of [10, 16]) {
    rectInclusive(d, xx, 13, xx + 2, sleepy ? 14 : 16, BLACK);
    if (!sleepy) surface.set(xx, 13, WHITE);
  }
  line(d, 12, 18, 15, 18, BLACK);

  const rainy =
    condition.includes("rain") ||
    condition === "pouring" ||
    condition === "lightning-rainy";

  if (rainy) {
    polygon(
      d,
      [
        { x: 2, y: 8 },
        { x: 6, y: 3 },
        { x: 18, y: 3 },
        { x: 23, y: 8 },
      ],
      YELLOW,
    );
    line(d, 12, 8, 12, 11, BLACK);
    for (const xx of [2, 23, 26]) line(d, xx, 11, xx - 1, 13, BLACK);
  } else if ((temp !== undefined && temp <= 4) || condition.includes("snow")) {
    rectInclusive(d, 8, 17, 19, 19, YELLOW);
    rectInclusive(d, 18, 18, 20, 23, YELLOW);
  } else if (sleepy) {
    polyline(d, [21, 3, 25, 3, 21, 7, 25, 7], BLACK);
  } else if (condition === "sunny" || condition === "partlycloudy") {
    rectInclusive(d, 20, 2, 24, 6, YELLOW);
    line(d, 22, 0, 22, 1, BLACK);
    line(d, 26, 4, 28, 4, BLACK);
  } else {
    rectInclusive(d, 4, 4, 15, 6, BLACK);
    rectInclusive(d, 7, 2, 12, 5, BLACK);
  }

  return surface.toSprite();
}

/** Draw the octopus at 2x, exactly as the composer pastes it. */
export function drawOctopus(
  fb: FrameBuffer,
  x: number,
  y: number,
  options: OctopusOptions,
): void {
  fb.blitSprite(x, y, octopusSprite(options), undefined, OCTOPUS_SCALE);
}

export const WEATHER_ICON_W = 28;
export const WEATHER_ICON_H = 24;

/** Port of weather_icon(): drawn straight onto the frame at native scale. */
export function drawWeatherIcon(
  fb: FrameBuffer,
  x: number,
  y: number,
  condition: string,
): void {
  if (condition === "sunny" || condition === "clear-night") {
    ellipseInclusive(fb, x + 6, y + 2, x + 20, y + 16, YELLOW, BLACK);
    if (condition === "clear-night") {
      ellipseInclusive(fb, x + 12, y, x + 23, y + 11, WHITE);
    }
    return;
  }

  ellipseInclusive(fb, x + 2, y + 6, x + 15, y + 17, BLACK);
  ellipseInclusive(fb, x + 9, y + 1, x + 23, y + 17, BLACK);
  rectInclusive(fb, x + 3, y + 11, x + 25, y + 17, BLACK);

  if (condition.includes("rain") || condition === "pouring") {
    for (const xx of [5, 13, 21]) {
      // PIL draws these at width=2; two adjacent runs reproduce the weight.
      line(fb, x + xx, y + 20, x + xx - 2, y + 23, BLACK);
      line(fb, x + xx + 1, y + 20, x + xx - 1, y + 23, BLACK);
    }
  } else if (condition.includes("snow")) {
    for (const xx of [5, 13, 21]) {
      rectInclusive(fb, x + xx, y + 21, x + xx + 1, y + 22, BLACK);
    }
  }
}
