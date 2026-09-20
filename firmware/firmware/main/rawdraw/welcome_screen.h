/**
 * @file welcome_screen.h
 * @brief The welcome screen's geometry, its graphics, and its line fitting.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS IS SPLIT OUT OF dashboard_renderer.cc
 * ----------------------------------------------
 * Four claims this screen makes are the ones that would be wrong, and only one
 * of them is about what the words say:
 *
 *   1. the QR encodes the device's own `http://<ip>/` and nothing else — never
 *      the pairing token, never anything a caller could substitute;
 *   2. it degrades to a bordered, same-sized box when there is no address, so
 *      the layout does not move and the panel does not keep a stale QR;
 *   3. the swatch row shows the four raw hardware pigments, in order, as
 *      literal colours — not theme tokens, not dithered, and not labelled;
 *   4. nothing drawn on this screen crosses x = kWelcomeSafeRight, and nothing
 *      is handed a y above kWelcomeSafeTop.
 *
 * The first three are decidable from the framebuffer and none of them needs a
 * font. The fourth needs one — until the decision of *what to draw* is
 * separated from the drawing, which is what FitWelcomeLine() does: it takes a
 * width function, so a host test can drive it with a ruler instead of Source
 * Han Sans and still pin the rule that a line is shortened or dropped rather
 * than run off the panel.
 *
 * That rule is here because the first hardware lot did not have it. The right
 * column was drawn at unchecked x positions and every line of it ran off the
 * right edge of the glass; see the photograph in the lot 3 notes.
 *
 * `DrawText` would pull the CJK font component and esp_heap_caps.h into the
 * host suite, which is a much larger undertaking; the drawing of text stays in
 * the renderer, and everything a host can check lives here. See
 * tests/host/test_welcome_screen.cc.
 *
 * Free of ESP-IDF, LVGL and the font engine, like the rest of this directory.
 */

#ifndef RAWDRAW_WELCOME_SCREEN_H
#define RAWDRAW_WELCOME_SCREEN_H

#include <stddef.h>

#include "rawdraw.h"

namespace rawdraw {

// ------------------------------------------------------------ safe bounds --

/**
 * @brief The box every mark on this screen stays inside.
 *
 * x is inset by 20 on both sides, which clears the panel's physical corner
 * dead-zones at every y because x never enters the excluded band. These are the
 * numbers the host suite sweeps every other constant in this header against.
 *
 * THESE ARE COORDINATES THIS SCREEN HANDS TO A DRAWING CALL, NOT ROWS OF INK
 * -------------------------------------------------------------------------
 * For a rectangle the two are the same thing. For text they are not, and the
 * lot-3 photograph is what settles it. Measured against the QR's own modules —
 * 25 of them at 5 px, so the code's ink occupies y 83..208 whatever anyone
 * believes about the rest of the screen — every *rectangle* on that panel
 * landed within 1.5 px of its constant: the rule at 56 at 57.5, the swatch rule
 * at 258 at 259, the chips at 262..290 at 262..290, and framebuffer row 0 at
 * the visible edge of the glass. So the panel is not cropped. The bezel takes
 * about three rows, not the fifty-six it would need to explain the title.
 *
 * Every *line of text* on the same panel landed with the bottom of its ink at
 * `y - 19.5`, in both faces: the detail column handed 64, 86 and 108 drew at
 * 44.7, 66.8 and 88.4, and the caption handed 234 drew at 214.5 — which is also
 * why "Scan to connect" is touching the QR in the photograph. DrawText()'s
 * contract says @p y is the top edge of the line; on this device it is about
 * twenty pixels below the bottom of the ink. Fixing that is a change to every
 * page this firmware draws, and nothing here can prove it without a second
 * device image, so this screen does not compensate for it and does not encode
 * it. It does the one thing that is decidable: it never hands a y to a text
 * call that has not been photographed rendering whole.
 *
 * kWelcomeSafeTop is therefore a refusal, not a bezel depth. 62 is the QR box's
 * own top — the one primitive on lot 3 that was photographed, scanned and
 * proved to land on visible glass — and the band above it is refused because
 * nobody has established which of the two faults above owns which of its rows.
 * The self-asserted 8 it replaces was never measured against anything.
 */
constexpr int kWelcomeSafeLeft = 20;
constexpr int kWelcomeSafeRight = 380;
constexpr int kWelcomeSafeTop = 62;
constexpr int kWelcomeSafeBottom = 292;

// ---------------------------------------------------------------- layout --

/**
 * @brief The product name — the first strong line of the right column.
 *
 * WHY THE TITLE IS HERE AND NOT IN A BAND OF ITS OWN
 * --------------------------------------------------
 * It has been drawn into the top band twice and lost twice: into `{20,14,...}`
 * on the first lot and into `{20,30,...}` on lot 3, whose photograph shows
 * `NOTE4C` reduced to a six-pixel smear against the top edge of the glass. Its
 * ink was where the header above says it would be — bottom at 30 - 19.5 ≈ 10,
 * top at about -7 — so the missing rows were not behind the bezel, they were
 * clipped off the framebuffer. A third nudge chosen by arithmetic would be the
 * same bet a third time.
 *
 * So the title stops being a band and becomes a line of the right column, at
 * the one y on this screen with a photograph behind it: 64 is where lot 3 drew
 * `4-color e-paper`, and that line is whole and legible in the image. The
 * column's other two lines follow it in the Regular face, which is the whole
 * hierarchy this screen needs — a name, an address, a state.
 *
 * `4-color e-paper` is what the title displaces, and it is the right line to
 * lose. The swatch row at the foot of the screen shows the four pigments as
 * themselves; a line of text claiming there are four of them is the caption to
 * a picture the reader is already looking at. Dropping it keeps the three lines
 * at their measured spacing instead of tightening them to fit a fourth.
 */
constexpr Rect kWelcomeTitleBox{196, 64, 184, 24};

/// The reserved square for the QR, or for the text that replaces it. Fixed so
/// the two states occupy exactly the same space and the screen does not jump.
/// Untouched by the title's move, and its top *is* kWelcomeSafeTop: the 168 px
/// side, the module size, the four-module quiet zone and the `http://<ip>/`
/// payload are the part of this screen that was photographed working, so a
/// phone that scanned the lot-3 panel scans this one.
constexpr Rect kWelcomeQrBox{20, 62, 168, 168};

/// How far inside kWelcomeQrBox the fallback's lines of text start, and how
/// many of them there are.
constexpr int kWelcomeQrTextInset = 12;
constexpr int kWelcomeQrTextLines = 3;
constexpr int kWelcomeQrTextLineH = 26;
constexpr int kWelcomeQrTextTop = 56;

/// One line under the QR, telling the reader what to do with it.
constexpr Rect kWelcomeCaptionBox{20, 234, 360, 16};

/**
 * @brief The rest of the right column: where this device is, and how it is
 *        doing. Two lines now that the title has taken the first one.
 *
 * Its right edge *is* kWelcomeSafeRight, and it starts below kWelcomeTitleBox
 * rather than sharing a line with it. Every string drawn into it goes through
 * FitWelcomeLine() against its width, which is the whole reason the column has
 * a width at all rather than a starting x. Its bottom stops above
 * OctopusBounds(), which is what keeps the mascot out of the text.
 */
constexpr Rect kWelcomeDetailBox{196, 90, 184, 38};
constexpr int kWelcomeDetailLineH = 22;
constexpr int kWelcomeDetailLines = 2;

/**
 * @brief The mascot, centred in the right column between the detail lines and
 *        the rule. Ink-centred; see OctopusInkBounds().
 *
 * The scale is 4 rather than the composer's 2 because the composer draws onto a
 * shared panel and this screen has a column to itself: at 2x the mark measured
 * 58x52 and read as a smudge from arm's length on the lot-3 device. At 4x the
 * sprite paints 116x104 at (234,130), which clears the detail column's last
 * line (bottom 128) and stops above the swatch rule at 258. The bitmap is not
 * touched — enlargement is the block size and nothing else — so the art stays
 * identical to `sprites.ts` and the golden grid still holds.
 */
constexpr Point kWelcomeOctopusCenter{288, 190};
constexpr int kWelcomeOctopusScale = 4;

/// The swatch row: four cells across the full content width, and the rule above
/// it that separates them from the rest. Bottom at 290, one clear pixel inside
/// kWelcomeSafeBottom, and photographed intact on lot 3 — chips at 262..290,
/// rule at 259 against its constant of 258. Nothing here moved.
constexpr int kWelcomeSwatchRuleY = 258;
constexpr Rect kWelcomeSwatchRow{20, 262, 360, 28};
constexpr int kWelcomeSwatchCount = 4;

// -------------------------------------------------------------- the payload --

/**
 * @brief Build the QR's payload from an IP address.
 *
 * @param ip  dotted quad from WifiManager, or empty when there is none.
 * @param out,cap receives `http://<ip>/`, NUL-terminated.
 * @return false, leaving @p out empty, for a null or empty @p ip, for one that
 *         will not fit @p cap, or for one carrying any character outside digits
 *         and dots.
 *
 * The last check is the structural one. This function takes an address, not a
 * URL and not a template, and the alphabet it accepts has no slash, no `?`, no
 * `@` and no letters — so there is no input to this screen that could make it
 * display something other than the device's own address, and nowhere a
 * credential could be appended by a caller that had one. An IPv6 literal is
 * refused rather than mangled: it cannot be written unbracketed into a URL, and
 * the fallback box is the honest answer.
 */
bool WelcomeQrUrl(const char* ip, char* out, size_t cap);

/**
 * @brief Draw the QR for @p ip into kWelcomeQrBox.
 *
 * @return false, having drawn nothing, when there is no usable address or the
 *         code would not be scannable at this size. The caller then draws the
 *         bordered fallback box below and its own explanatory text.
 */
bool DrawWelcomeQr(uint8_t* fb, int width, int height, const char* ip);

/// The bordered placeholder that stands in for the QR. Same box, so the two
/// states are the same shape and a stale QR cannot survive under new text.
void DrawWelcomeQrFallbackBox(uint8_t* fb, int width, int height);

// ---------------------------------------------------------- line fitting --

/**
 * @brief How wide @p text would be, in pixels, in the caller's font.
 *
 * The renderer passes a thunk around MeasureTextWidth bound to Source Han Sans;
 * the host suite passes a fixed-width ruler. Neither this header nor its .cc
 * knows what a font is, which is the point.
 */
using TextWidthFn = int (*)(const char* text, void* ctx);

/// Written between the truncated text and the edge when a line has to be cut.
/// Three ASCII dots rather than U+2026, which the slim font subset lacks.
constexpr const char* kWelcomeEllipsis = "...";

/**
 * @brief Decide what to put on one line of this screen, given how much room it
 *        has and how wide things are.
 *
 * @param candidates  what to say, longest and most informative first; each a
 *                    complete, sensible sentence on its own.
 * @param count       how many.
 * @param max_width   the pixels available. Nothing wider is ever returned.
 * @param measure,ctx the font.
 * @param out,cap     receives the chosen text, NUL-terminated.
 * @return false, leaving @p out empty, when nothing at all fits — in which case
 *         the caller draws no line rather than an overflowing one.
 *
 * The first candidate that fits whole wins. If none does, the *last* one — the
 * shortest, the one written for this case — is truncated and given an ellipsis.
 * If even one character plus the ellipsis is too wide, the line is dropped.
 *
 * WHY CANDIDATES RATHER THAN JUST TRUNCATION
 * ------------------------------------------
 * "Ready for a das..." tells a person less than "Ready" does, and looks like a
 * bug besides. The copy for a narrow column is a writing problem, so the writer
 * supplies the short forms and this function picks; truncation is the last
 * resort for the one string nobody can write short, the device's own address.
 *
 * Multi-byte text is never cut inside a character: the truncation point is
 * walked back off any UTF-8 continuation byte. The strings this screen draws
 * are ASCII, but a mangled final byte would be a rendering fault that only
 * appeared in a language nobody ran the tests in.
 */
bool FitWelcomeLine(const char* const* candidates, int count, int max_width,
                    TextWidthFn measure, void* ctx, char* out, size_t cap);

// ---------------------------------------------------------- the swatch row --

/**
 * @brief One pigment chip.
 *
 * No label field, and that is deliberate. The row's job is to show what the
 * four pigments look like on this glass; a person can see that a square is
 * yellow without being told, and the words that used to sit under these chips
 * were the only text on the screen that carried no information at all. The
 * first hardware lot printed them overlapping the chips themselves.
 */
struct PaletteSwatch {
    Color color;
    /// WHITE needs two pixels to be visible against a white canvas; the rest
    /// take one, so the four read as a set.
    int border_px;
    Rect chip;
    /// The cell the chip is centred in. Kept because it is what makes the
    /// spacing checkable, not because anything is drawn in it.
    int cell_x;
    int cell_w;
};

/// Fill @p out with the four swatches, in hardware order.
void WelcomeSwatchLayout(PaletteSwatch out[kWelcomeSwatchCount]);

/// Draw the four chips and their borders. Nothing else: no labels, no text.
void DrawWelcomeSwatches(uint8_t* fb, int width, int height);

}  // namespace rawdraw

#endif  // RAWDRAW_WELCOME_SCREEN_H
