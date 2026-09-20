import {
  FRAME_HEIGHT,
  FRAME_PIXELS,
  FRAME_WIDTH,
  PACKED_BYTES,
  WHITE,
  type PaletteIndex,
} from "./palette";
import {
  clean,
  decodeGlyph,
  fitText,
  measureRun,
  resolveRun,
  type FontAtlas,
} from "./font";
import { TRANSPARENT, pictogramMask } from "./render/pictograms";

export class NonPalettePixelError extends Error {
  readonly offset: number;
  readonly value: number;
  constructor(offset: number, value: number) {
    super(
      `Non-palette pixel ${value} at offset ${offset}. Only 0 black, 1 white, 2 yellow, 3 red may reach the device.`,
    );
    this.name = "NonPalettePixelError";
    this.offset = offset;
    this.value = value;
  }
}

export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface Sprite {
  w: number;
  h: number;
  /** One palette index per pixel, row-major. */
  data: Uint8Array;
}

export interface DrawTextOptions {
  /** Truncate with an ellipsis to this width, as the composer's fit() does. */
  maxWidth?: number;
  /** "left" places x at the pen origin; "right" and "center" measure first. */
  align?: "left" | "center" | "right";
}

/**
 * A 400x300 buffer of palette indices. Every primitive writes indices only,
 * never RGB, so pack() can enforce the palette as a hard gate.
 */
export class FrameBuffer {
  static readonly WIDTH = FRAME_WIDTH;
  static readonly HEIGHT = FRAME_HEIGHT;

  readonly width = FRAME_WIDTH;
  readonly height = FRAME_HEIGHT;
  readonly pixels: Uint8Array;

  constructor(fill: PaletteIndex = WHITE) {
    this.pixels = new Uint8Array(FRAME_PIXELS);
    if (fill !== 0) this.pixels.fill(fill);
  }

  static from(pixels: Uint8Array): FrameBuffer {
    if (pixels.length !== FRAME_PIXELS) {
      throw new RangeError(
        `Expected ${FRAME_PIXELS} pixels, received ${pixels.length}`,
      );
    }
    const fb = new FrameBuffer();
    fb.pixels.set(pixels);
    return fb;
  }

  clone(): FrameBuffer {
    return FrameBuffer.from(this.pixels);
  }

  /** Out-of-bounds writes are clipped: modules draw near edges routinely. */
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

  fill(color: PaletteIndex): void {
    this.pixels.fill(color);
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

  /** 1 px outline drawn inside the rect. */
  strokeRect(
    x: number,
    y: number,
    w: number,
    h: number,
    color: PaletteIndex,
  ): void {
    if (w <= 0 || h <= 0) return;
    this.hline(x, y, w, color);
    this.hline(x, y + h - 1, w, color);
    this.vline(x, y, h, color);
    this.vline(x + w - 1, y, h, color);
  }

  hline(x: number, y: number, length: number, color: PaletteIndex): void {
    this.fillRect(x, y, length, 1, color);
  }

  vline(x: number, y: number, length: number, color: PaletteIndex): void {
    this.fillRect(x, y, 1, length, color);
  }

  /**
   * Blit a sprite of palette indices. transparent, when given, is the sprite
   * index treated as see-through (the octopus port uses white for this).
   */
  blitSprite(
    x: number,
    y: number,
    sprite: Sprite,
    transparent?: PaletteIndex,
    scale = 1,
  ): void {
    const step = Math.max(1, Math.trunc(scale));
    for (let sy = 0; sy < sprite.h; sy += 1) {
      for (let sx = 0; sx < sprite.w; sx += 1) {
        const value = sprite.data[sy * sprite.w + sx] as PaletteIndex;
        if (transparent !== undefined && value === transparent) continue;
        if (step === 1) {
          this.set(x + sx, y + sy, value);
        } else {
          this.fillRect(x + sx * step, y + sy * step, step, step, value);
        }
      }
    }
  }

  /**
   * Draw text with a pre-rasterised atlas. x, y is the left-ascender origin,
   * matching Pillow's default anchor so ported layout coordinates carry over
   * unchanged. Returns the pen width actually drawn.
   */
  drawText(
    atlas: FontAtlas,
    x: number,
    y: number,
    value: string,
    color: PaletteIndex,
    options: DrawTextOptions = {},
  ): number {
    const text =
      options.maxWidth !== undefined
        ? fitText(atlas, value, options.maxWidth)
        : clean(value);
    if (text.length === 0) return 0;

    const { items } = resolveRun(atlas, text);
    const width = measureRun(items);
    let pen = x;
    if (options.align === "right") pen = x - width;
    else if (options.align === "center") pen = x - width / 2;

    for (const item of items) {
      if (item.kind === "picto") {
        // A pictogram carries its own pigments, so it ignores the text colour.
        // Its bottom sits on the same line as the letters' baseline; if it is
        // taller than the ascender it is pinned to the top of the line rather
        // than allowed to climb into the row above.
        const mask = pictogramMask(item.picto, item.scale);
        const originX = Math.round(pen);
        const originY = Math.round(y) + Math.max(0, atlas.ascent - item.side);
        for (let py = 0; py < mask.h; py += 1) {
          const targetY = originY + py;
          if (targetY < 0 || targetY >= this.height) continue;
          for (let px = 0; px < mask.w; px += 1) {
            const value = mask.data[py * mask.w + px] as number;
            if (value === TRANSPARENT) continue;
            const targetX = originX + px;
            if (targetX < 0 || targetX >= this.width) continue;
            this.pixels[targetY * this.width + targetX] = value;
          }
        }
        pen += item.advance;
        continue;
      }

      const decoded = decodeGlyph(item.glyph);
      const originX = Math.round(pen) + decoded.l;
      const originY = Math.round(y) + decoded.t;
      for (let gy = 0; gy < decoded.h; gy += 1) {
        const rowBase = gy * decoded.w;
        const targetY = originY + gy;
        if (targetY < 0 || targetY >= this.height) continue;
        for (let gx = 0; gx < decoded.w; gx += 1) {
          if (decoded.mask[rowBase + gx] === 0) continue;
          const targetX = originX + gx;
          if (targetX < 0 || targetX >= this.width) continue;
          this.pixels[targetY * this.width + targetX] = color;
        }
      }
      pen += item.advance;
    }
    return width;
  }
}

/**
 * The acceptance gate for everything this product renders: exactly 30000
 * bytes, 2 bits per pixel, MSB first, four pixels per byte, in device palette
 * order. Any pixel outside 0..3 is a bug and must never reach the panel.
 */
export function pack(fb: FrameBuffer | Uint8Array): Uint8Array {
  const pixels = fb instanceof FrameBuffer ? fb.pixels : fb;
  if (pixels.length !== FRAME_PIXELS) {
    throw new RangeError(
      `Expected a ${FRAME_WIDTH}x${FRAME_HEIGHT} frame (${FRAME_PIXELS} pixels), received ${pixels.length}`,
    );
  }
  const out = new Uint8Array(PACKED_BYTES);
  for (let i = 0; i < FRAME_PIXELS; i += 1) {
    const value = pixels[i] as number;
    if (value > 3) throw new NonPalettePixelError(i, value);
    const byteIndex = i >> 2;
    out[byteIndex] = (out[byteIndex] as number) | (value << (6 - 2 * (i & 3)));
  }
  return out;
}

export function unpack(bytes: Uint8Array): FrameBuffer {
  if (bytes.length !== PACKED_BYTES) {
    throw new RangeError(
      `Expected ${PACKED_BYTES} packed bytes, received ${bytes.length}`,
    );
  }
  const fb = new FrameBuffer();
  for (let i = 0; i < FRAME_PIXELS; i += 1) {
    const byte = bytes[i >> 2] as number;
    fb.pixels[i] = (byte >> (6 - 2 * (i & 3))) & 3;
  }
  return fb;
}
