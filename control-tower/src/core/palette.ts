/**
 * The device palette contract. This ordering is law and comes from the panel
 * itself, not from a theme choice:
 *
 *   black = 0, white = 1, yellow = 2, red = 3
 *
 * Evidence: note4c-firmware/upstream/firmware/docs/DASHBOARD_API.md, the
 * PUT /api/v1/dashboard/frame body description. The device copies the frame
 * without conversion, so any other ordering paints the wrong colours.
 */

export const BLACK = 0 as const;
export const WHITE = 1 as const;
export const YELLOW = 2 as const;
export const RED = 3 as const;

export type PaletteIndex = 0 | 1 | 2 | 3;

export const PALETTE_INDICES: readonly PaletteIndex[] = [BLACK, WHITE, YELLOW, RED];

export const PALETTE_NAMES = ["black", "white", "yellow", "red"] as const;
export type PaletteName = (typeof PALETTE_NAMES)[number];

/**
 * Preview colours for the browser and for PNG export. These are the pure
 * values the composer uses (note4c-dashboard/dashboard.py PALETTE), not the
 * paper-styled UI tokens. The preview must never soften them.
 */
export const PREVIEW_RGB: readonly string[] = [
  "#000000",
  "#FFFFFF",
  "#FFFF00",
  "#FF0000",
];

export const PREVIEW_RGB_TRIPLETS: readonly (readonly [number, number, number])[] = [
  [0, 0, 0],
  [255, 255, 255],
  [255, 255, 0],
  [255, 0, 0],
];

export const FRAME_WIDTH = 400;
export const FRAME_HEIGHT = 300;
export const FRAME_PIXELS = FRAME_WIDTH * FRAME_HEIGHT; // 120000
export const PACKED_BYTES = FRAME_PIXELS / 4; // 30000, 2 bits per pixel

export function isPaletteIndex(value: number): value is PaletteIndex {
  return value === 0 || value === 1 || value === 2 || value === 3;
}

export function paletteName(index: PaletteIndex): PaletteName {
  return PALETTE_NAMES[index];
}
