/**
 * @file octopus_mark.h
 * @brief The house octopus, as pixel art, at integer scale.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHERE THE ARTWORK COMES FROM
 * ----------------------------
 * This is the mascot the project has drawn since the Python composer: a 29x26
 * sprite whose body is a thirteen-column bitmap and whose face is three
 * rectangles. It is transcribed pixel for pixel from
 * `note4c-control-tower-autonomy/src/core/render/sprites.ts`
 * (`OCTOPUS_BODY` and the non-sleepy branch of `octopusSprite`), which is
 * itself a port of `note4c-dashboard/dashboard.py`'s `octopus()`.
 *
 * An earlier version of this file drew a circle and eight polylines instead.
 * It was a different animal — on the glass it read as a black spider — and the
 * first device that shipped it made that obvious. The lesson is narrow and
 * worth writing down: this mark is an *asset*, not a shape to re-derive. If it
 * needs to change, it changes in the bitmap below and in sprites.ts together.
 *
 * WHY IT IS CODE AND NOT A GLYPH
 * ------------------------------
 * Neither font system on this board has emoji coverage: not Source Han Sans,
 * not the Inter subset in autonomy_font_data, not the icon fonts. Adding one
 * would mean a font-generation pipeline change and a few hundred kilobytes of
 * flash for a single picture on a single screen. A 29x26 bitmap costs 754
 * bytes of rodata.
 *
 * THE PLAIN FORM ONLY
 * -------------------
 * `octopusSprite()` also draws a weather accessory — a cloud, a sun, an
 * umbrella, a scarf, a sleeping Z — chosen from a forecast. None of that is
 * drawn here. The welcome screen runs before the device has a forecast, or a
 * dashboard, or in most cases a network, so an accessory would be an assertion
 * about weather this code cannot make. What is left is the body, two eyes and
 * a mouth, which is exactly the plain form.
 *
 * COLOUR, AND WHAT A BLACK-AND-WHITE PANEL DOES TO IT
 * --------------------------------------------------
 * The body is RED, which is the mascot's colour and one of the four pigments
 * the welcome screen's swatch row is there to demonstrate. On a
 * CONFIG_DISPLAY_EPD_1BPP build the red collapses to black along with the
 * eyes, and what survives is a black silhouette with one white highlight pixel
 * per eye. That is a worse picture but still an octopus, and it is the honest
 * consequence of the panel rather than something this file can paper over by
 * drawing the mascot in the wrong colour everywhere.
 *
 * Free of ESP-IDF, LVGL and the font engine, like the rest of this directory,
 * so tests/host/test_octopus_mark.cc compiles this exact translation unit.
 */

#ifndef RAWDRAW_OCTOPUS_MARK_H
#define RAWDRAW_OCTOPUS_MARK_H

#include "rawdraw.h"

namespace rawdraw {

/// The sprite grid, matching OCTOPUS_W/OCTOPUS_H in sprites.ts. The margin
/// around the body is where the weather accessory would go; here it is white.
constexpr int kOctopusSpriteW = 29;
constexpr int kOctopusSpriteH = 26;

/// The part of that grid the plain form actually inks: the body bitmap, pasted
/// at (7, 9) by `octopusSprite()`. The eyes and the mouth fall inside it, so
/// this rectangle is the whole of the drawing a person sees.
constexpr Rect kOctopusInk{7, 9, 13, 12};

/// The block size this screen uses: each source pixel becomes a scale x scale
/// block, so the edges stay hard and the output is identical on every build.
///
/// Raised from the composer's `OCTOPUS_SCALE` of 2 after the lot-3 photograph:
/// 29x26 source pixels at 2x is 58x52 on a 400x300 panel, which read as a
/// smudge rather than as an octopus. Nothing about the *art* changed — a
/// bitmap edit would still have to be made here and in `sprites.ts` together —
/// only how many device pixels one source pixel occupies.
/// kWelcomeOctopusScale is pinned equal to this by the host suite.
constexpr int kOctopusScale = 4;

/// Below this there is no sprite left to draw. Integer scales only — a
/// fractional one would resample the art and soften exactly the edges that
/// make it read as pixel art.
constexpr int kOctopusMinScale = 1;

/**
 * @brief The rectangle DrawOctopusMark() paints, for a mark whose ink is
 *        centred on @p ink_center at @p scale.
 *
 * The full sprite, white margin included: the mark clears its own box, which is
 * what lets the welcome screen be redrawn over a framebuffer that still holds
 * the previous render.
 */
Rect OctopusBounds(const Point& ink_center, int scale);

/**
 * @brief The inked part of that rectangle — the body and face alone.
 *
 * Exposed because centring on the sprite box would sit the visible octopus
 * four pixels low and two pixels left, the accessory margin being asymmetric.
 * Callers centre on this; tests assert against it.
 */
Rect OctopusInkBounds(const Point& ink_center, int scale);

/**
 * @brief Draw the plain mascot with its ink centred on @p ink_center.
 *
 * Draws nothing at all when @p fb is null, when @p scale is below
 * kOctopusMinScale, or when OctopusBounds() would fall outside the
 * framebuffer — refused rather than clipped, because half an octopus at the
 * edge of the panel looks like a rendering fault, and nothing at all is a
 * layout the caller can see is wrong.
 *
 * Every pixel it draws is inside OctopusBounds() and is RED, BLACK or WHITE.
 * Deterministic: no randomness, no time, no statics.
 */
void DrawOctopusMark(uint8_t* fb, int width, int height, const Point& ink_center,
                     int scale);

}  // namespace rawdraw

#endif  // RAWDRAW_OCTOPUS_MARK_H
