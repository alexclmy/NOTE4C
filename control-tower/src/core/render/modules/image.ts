import { z } from "zod";
import type { FrameBuffer } from "@/core/frame";
import type { PaletteIndex } from "@/core/palette";
import { decodeTile } from "../imageDither";
import type { ModuleDefinition, PixelRect } from "../types";
import { clearModule, renderUnavailable, unavailableShape } from "./common";

/**
 * A photo, dithered to the panel's four colours.
 *
 * HOW THE PICTURE GETS HERE
 * -------------------------
 * The owner drops an image in the inspector. The browser resizes it to this
 * tile on a `<canvas>`, dithers it to palette indices (see ../dither), and
 * stores the RESULT — the indices, packed to 2 bits per pixel and base64'd —
 * in this module's options. Nothing else about the image is kept: not the
 * original bytes, not a URL. The picture never leaves the machine, and the
 * document stays small because it carries a sub-tile, not a photograph.
 *
 * WHY THE RENDERER ONLY BLITS
 * ---------------------------
 * Rendering decodes those stored indices and copies them into the frame. It
 * does not decode an image, quantise, or dither, so it needs no native library
 * and it is deterministic: the server render reproduces the browser's result
 * byte for byte because it is replaying the browser's result, not recomputing
 * it. The four indices it writes are the only four the panel accepts, so it
 * passes pack()'s gate unchanged.
 *
 * WHAT IT WILL NOT DO
 * -------------------
 * With no image chosen it renders an explicit "add a photo" state, in words
 * the owner can edit, rather than a blank rectangle — an empty tile on frozen
 * ink reads as a module that failed to draw.
 */

/** Marker so the inspector gives this a real upload widget, not a text box. */
export const IMAGE_SOURCE_TAG = "image-source";

export const IMAGE_FITS = ["cover", "contain"] as const;
export type ImageFit = (typeof IMAGE_FITS)[number];

export const IMAGE_MODES = ["photo", "poster"] as const;
export type ImageMode = (typeof IMAGE_MODES)[number];

/** Full-panel tile is 400x300 -> 30000 packed bytes -> ~40000 base64 chars. */
export const MAX_ENCODED_TILE = 40008;

/**
 * The picture and the choices that produced it, in one object so the inspector
 * can own the whole upload-and-dither flow behind a single widget.
 *
 * `data` is the dithered tile: base64 of `tileW*tileH` palette indices packed
 * 2 bits per pixel. `tileW`/`tileH` are the pixel size it was dithered at,
 * which the browser sets to the module's own pixel tile so a fresh dither lands
 * 1:1. The dither knobs (`fit`, `mode`, `colourAmount`, `contrast`) are stored
 * so the inspector can re-run the dither from the in-memory source when the
 * owner nudges them, and so a reader can see how the picture was made.
 */
export const ImageSourceSchema = z
  .object({
    fit: z.enum(IMAGE_FITS).default("cover"),
    mode: z.enum(IMAGE_MODES).default("photo"),
    colourAmount: z.number().int().min(0).max(100).default(60),
    contrast: z.number().int().min(-100).max(100).default(0),
    tileW: z.number().int().min(0).max(400).default(0),
    tileH: z.number().int().min(0).max(300).default(0),
    data: z.string().max(MAX_ENCODED_TILE).nullable().default(null),
  })
  .describe(IMAGE_SOURCE_TAG);
export type ImageSource = z.infer<typeof ImageSourceSchema>;

export const ImageOptions = z.object({
  source: ImageSourceSchema.default({}),
  ...unavailableShape({
    title: "Photo",
    note: "Ajoutez une image",
  }),
});
export type ImageOptions = z.infer<typeof ImageOptions>;

/**
 * Copy a decoded tile into the frame, clipped to the module rectangle and
 * centred within it. Centring only matters when the stored tile no longer
 * matches the rectangle — the owner resized the module without re-dithering —
 * in which case the picture keeps its pixels rather than being scaled, which
 * would blur the dither and could re-introduce isolated accents. Clipping to
 * the rect keeps a too-large tile from bleeding onto a neighbour.
 */
function blitTile(
  fb: FrameBuffer,
  rect: PixelRect,
  tile: Uint8Array,
  tileW: number,
  tileH: number,
): void {
  const offsetX = rect.x + Math.floor((rect.w - tileW) / 2);
  const offsetY = rect.y + Math.floor((rect.h - tileH) / 2);
  for (let ty = 0; ty < tileH; ty += 1) {
    const py = offsetY + ty;
    if (py < rect.y || py >= rect.y + rect.h) continue;
    for (let tx = 0; tx < tileW; tx += 1) {
      const px = offsetX + tx;
      if (px < rect.x || px >= rect.x + rect.w) continue;
      fb.set(px, py, (tile[ty * tileW + tx] as number) as PaletteIndex);
    }
  }
}

export const image: ModuleDefinition<ImageOptions, never> = {
  type: "image",
  label: "Image",
  description:
    "A photo you upload, dithered to the panel's four colours in the browser and stored as the finished tile. The picture never leaves this machine, and the renderer only blits the stored result.",
  schema: ImageOptions,
  defaultOptions: ImageOptions.parse({}),
  defaultSpan: { w: 3, h: 3 },
  minSpan: { w: 1, h: 1 },
  maxSpan: { w: 8, h: 6 },
  sourceBinding: "none",

  render(fb, rect, _data, options, ctx) {
    clearModule(fb, rect);
    const { source } = options;

    if (!source.data || source.tileW <= 0 || source.tileH <= 0) {
      // No picture yet: say so, rather than leaving a blank the reader would
      // mistake for a module that failed to paint.
      renderUnavailable(fb, rect, options, ctx.report);
      return;
    }

    const tile = decodeTile(source.data, source.tileW, source.tileH);
    blitTile(fb, rect, tile, source.tileW, source.tileH);
  },
};
