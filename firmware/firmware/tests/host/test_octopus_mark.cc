/**
 * @file test_octopus_mark.cc
 * @brief Host tests for main/rawdraw/octopus_mark.cc.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS SUITE HAS A GOLDEN AND THE PREVIOUS ONE DID NOT
 * -------------------------------------------------------
 * The mark used to be circles and polylines, and a golden image over a drawing
 * that is being tuned is a test that gets regenerated without being read. So
 * the old suite pinned only properties — bounds, palette, determinism — and
 * left the picture free.
 *
 * That freedom is exactly what went wrong. The picture drifted into a black
 * spider, shipped on hardware lot 3, and no test failed, because every property
 * still held. The mark is not a drawing being tuned; it is the project's mascot,
 * transcribed from `note4c-control-tower-autonomy/src/core/render/sprites.ts`,
 * and the thing worth pinning is that it still *is* that sprite.
 *
 * So kGolden below is the whole 29x26 grid, written out. If it has to change,
 * it changes here and in sprites.ts in the same breath, and the diff shows a
 * reader precisely which pixels moved. The properties are still tested too —
 * they are cheap, and each one is still a real failure mode.
 */

#include "rawdraw/octopus_mark.h"

#include "rawdraw/welcome_screen.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace rawdraw;

// ------------------------------------------------------------ mini harness --

static int g_checks = 0;
static int g_failures = 0;
static const char* g_current_test = "";

static void Check(bool cond, const char* expr, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] %s\n", g_current_test, line, expr);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

#define RUN(fn)                                                    \
    do {                                                           \
        g_current_test = #fn;                                      \
        const int before = g_failures;                             \
        fn();                                                      \
        std::printf("%-58s %s\n", #fn,                             \
                    (g_failures == before) ? "ok" : "FAILED");     \
    } while (0)

// --------------------------------------------------------------- fixtures --

constexpr int kW = 400;
constexpr int kH = 300;
constexpr size_t kFbBytes = static_cast<size_t>((kW * 2 + 7) / 8) * kH;

struct Canvas {
    std::vector<uint8_t> fb;
    Canvas() : fb(kFbBytes, 0) { Clear(fb.data(), kW, kH, WHITE); }
    uint8_t* data() { return fb.data(); }
    Color at(int x, int y) const { return get_pixel(fb.data(), kW, x, y); }
};

/**
 * @brief The mascot, one character per source pixel.
 *
 * `.` white, `R` red, `K` black. Rows 0..8 are the margin `octopusSprite()`
 * reserves for a weather accessory; the plain form leaves them empty, and
 * test_no_weather_accessory_is_drawn reads them back to say so.
 *
 * The two `.` inside the eyes on row 13 are the highlight pixels — sprites.ts
 * writes the eye as a solid black rectangle and then sets its top-left pixel
 * back to white. They are the only part of the face that survives a
 * black-and-white panel, so they are not a rounding artefact to tidy away.
 */
static const char* const kGolden[kOctopusSpriteH] = {
    ".............................",
    ".............................",
    ".............................",
    ".............................",
    ".............................",
    ".............................",
    ".............................",
    ".............................",
    ".............................",
    "..........RRRRRRR............",
    ".........RRRRRRRRR...........",
    "........RRRRRRRRRRR..........",
    ".......RRRRRRRRRRRRR.........",
    ".......RRR.KKRRR.KKR.........",
    ".......RRRKKKRRRKKKR.........",
    ".......RRRKKKRRRKKKR.........",
    "........RRKKKRRRKKK..........",
    ".........RRRRRRRRR...........",
    "........RR.RKKKK.RR..........",
    ".......RR..RR.RR..RR.........",
    ".......R..RR...RR..R.........",
    ".............................",
    ".............................",
    ".............................",
    ".............................",
    ".............................",
};

static Color GoldenAt(int sx, int sy) {
    switch (kGolden[sy][sx]) {
        case 'R': return RED;
        case 'K': return BLACK;
        default:  return WHITE;
    }
}

/// Where the suite draws when it does not care about the welcome screen's own
/// placement. Far enough from every edge that no refusal is triggered.
static constexpr Point kCentre{200, 150};

// ------------------------------------------------------------ the artwork --

/// The golden itself is well-formed: 29 columns on every one of 26 rows. A row
/// that lost a character would otherwise read past its end below.
static void test_the_golden_is_the_declared_grid() {
    for (int y = 0; y < kOctopusSpriteH; ++y) {
        CHECK(std::strlen(kGolden[y]) == static_cast<size_t>(kOctopusSpriteW));
    }
}

static void test_the_mascot_matches_the_historical_sprite() {
    Canvas c;
    DrawOctopusMark(c.data(), kW, kH, kCentre, 1);
    const Rect b = OctopusBounds(kCentre, 1);
    CHECK(b.w == kOctopusSpriteW);
    CHECK(b.h == kOctopusSpriteH);

    for (int sy = 0; sy < kOctopusSpriteH; ++sy) {
        for (int sx = 0; sx < kOctopusSpriteW; ++sx) {
            const Color got = c.at(b.x + sx, b.y + sy);
            if (got != GoldenAt(sx, sy)) {
                std::printf("  (sprite pixel %d,%d: got %d, want %c)\n", sx, sy,
                            static_cast<int>(got), kGolden[sy][sx]);
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

/**
 * SCALING IS A BLOCK FILL, NOT A RESAMPLE.
 *
 * The art is pixel art; a filter that softened the edges would make it a
 * picture of an octopus rather than the mascot. Checked at the scale the screen
 * uses and at one above it, so the arithmetic is exercised rather than the one
 * case that happens to be in production.
 */
static void test_scaling_is_a_block_fill() {
    for (int scale = 1; scale <= 3; ++scale) {
        Canvas c;
        DrawOctopusMark(c.data(), kW, kH, kCentre, scale);
        const Rect b = OctopusBounds(kCentre, scale);
        CHECK(b.w == kOctopusSpriteW * scale);
        CHECK(b.h == kOctopusSpriteH * scale);

        bool ok = true;
        for (int y = 0; y < b.h && ok; ++y) {
            for (int x = 0; x < b.w; ++x) {
                if (c.at(b.x + x, b.y + y) != GoldenAt(x / scale, y / scale)) {
                    std::printf("  (scale %d: pixel %d,%d)\n", scale, x, y);
                    ok = false;
                    break;
                }
            }
        }
        CHECK(ok);
    }
}

/// The welcome screen's actual call. The two scales stay equal — the screen
/// draws the composer's mark, not a variant of it — and the lot-3 photograph
/// is why the shared value is now 4 rather than 2: at 58x52 on a 400x300 panel
/// the mascot read as a smudge. The bitmap is untouched; only the block size
/// changed, so 29x26 sprite pixels paint 116x104.
static void test_the_welcome_screen_draws_it_at_the_composer_scale() {
    CHECK(kWelcomeOctopusScale == kOctopusScale);
    const Rect b = OctopusBounds(kWelcomeOctopusCenter, kWelcomeOctopusScale);
    CHECK(b.w == 116);
    CHECK(b.h == 104);
}

/**
 * THE BODY IS RED.
 *
 * The lot 3 device drew it in black, which is what a mark built from BLACK-only
 * primitives can do. Counted rather than sampled: a single red pixel would pass
 * a probe, and a mostly-red canvas would pass a "there is red" check.
 */
static void test_the_body_is_red_and_the_face_is_black() {
    Canvas c;
    DrawOctopusMark(c.data(), kW, kH, kCentre, 1);
    const Rect b = OctopusBounds(kCentre, 1);

    int red = 0, black = 0, white = 0, other = 0;
    for (int y = b.y; y < b.y + b.h; ++y) {
        for (int x = b.x; x < b.x + b.w; ++x) {
            switch (c.at(x, y)) {
                case RED:   ++red;   break;
                case BLACK: ++black; break;
                case WHITE: ++white; break;
                default:    ++other; break;
            }
        }
    }
    // The body bitmap, less the pixels the eyes and the mouth punch out of it.
    CHECK(red == 94);
    // Two 3x4 eyes with a highlight pixel each, and a four-pixel mouth.
    CHECK(black == 26);
    CHECK(other == 0);
    CHECK(white == kOctopusSpriteW * kOctopusSpriteH - red - black);
}

/**
 * NO YELLOW, ANYWHERE.
 *
 * Yellow only enters `octopusSprite()` through a weather accessory — the sun,
 * the umbrella, the scarf. Its absence is the cheapest single check that the
 * plain form is what got drawn.
 */
static void test_only_three_pigments_appear() {
    Canvas c;
    DrawOctopusMark(c.data(), kW, kH, kCentre, 2);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Color p = c.at(x, y);
            if (p != BLACK && p != WHITE && p != RED) {
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

/// The other half of the same claim, stated positionally: the band above the
/// body — where the cloud, the sun and the Z would go — is blank.
static void test_no_weather_accessory_is_drawn() {
    Canvas c;
    DrawOctopusMark(c.data(), kW, kH, kCentre, 2);
    const Rect b = OctopusBounds(kCentre, 2);
    const Rect ink = OctopusInkBounds(kCentre, 2);

    for (int y = b.y; y < ink.y; ++y) {
        for (int x = b.x; x < b.x + b.w; ++x) {
            if (c.at(x, y) != WHITE) {
                std::printf("  (accessory band inked at %d,%d)\n", x, y);
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

// -------------------------------------------------------------- geometry --

/**
 * THE BOUNDS ARE A PROMISE, AND THE INK BOUNDS ARE THE TIGHTER ONE.
 *
 * The welcome screen centres the mark on its ink, not on the sprite box, so
 * both rectangles have to mean what they say. Swept over the supported scales
 * because the centring arithmetic halves an odd product at scale 1 and 3.
 */
static void test_no_pixel_escapes_the_ink_bounds() {
    for (int scale = kOctopusMinScale; scale <= 6; ++scale) {
        Canvas c;
        DrawOctopusMark(c.data(), kW, kH, kCentre, scale);
        const Rect ink = OctopusInkBounds(kCentre, scale);

        bool escaped = false;
        for (int y = 0; y < kH && !escaped; ++y) {
            for (int x = 0; x < kW; ++x) {
                const bool inside = x >= ink.x && x < ink.x + ink.w &&
                                    y >= ink.y && y < ink.y + ink.h;
                if (!inside && c.at(x, y) != WHITE) {
                    std::printf("  (scale %d: pixel at %d,%d outside ink %d,%d %dx%d)\n",
                                scale, x, y, ink.x, ink.y, ink.w, ink.h);
                    escaped = true;
                    break;
                }
            }
        }
        CHECK(!escaped);
    }
}

static void test_the_ink_is_centred_and_the_sprite_contains_it() {
    for (int scale = kOctopusMinScale; scale <= 6; ++scale) {
        const Rect ink = OctopusInkBounds(kCentre, scale);
        const Rect b = OctopusBounds(kCentre, scale);

        CHECK(ink.w == kOctopusInk.w * scale);
        CHECK(ink.h == kOctopusInk.h * scale);
        CHECK(ink.x + ink.w / 2 == kCentre.x);
        CHECK(ink.y + ink.h / 2 == kCentre.y);

        CHECK(b.x <= ink.x);
        CHECK(b.y <= ink.y);
        CHECK(b.x + b.w >= ink.x + ink.w);
        CHECK(b.y + b.h >= ink.y + ink.h);
    }
}

// ------------------------------------------------------------- behaviour --

static void test_the_same_call_draws_the_same_bytes() {
    Canvas a;
    Canvas b;
    DrawOctopusMark(a.data(), kW, kH, kWelcomeOctopusCenter, kWelcomeOctopusScale);
    DrawOctopusMark(b.data(), kW, kH, kWelcomeOctopusCenter, kWelcomeOctopusScale);
    CHECK(a.fb == b.fb);

    Canvas d;
    DrawOctopusMark(d.data(), kW, kH, kWelcomeOctopusCenter, 3);
    CHECK(a.fb != d.fb);
}

/// Every refusal leaves the canvas exactly as it found it.
static void test_refusals_draw_nothing() {
    struct Case { Point where; int scale; const char* why; };
    const Case cases[] = {
        {kCentre, kOctopusMinScale - 1, "below the minimum scale"},
        {kCentre, 0, "zero scale"},
        {kCentre, -2, "negative scale"},
        {Point{10, 150}, 2, "off the left edge"},
        {Point{200, 10}, 2, "off the top edge"},
        {Point{395, 150}, 2, "off the right edge"},
        {Point{200, 295}, 2, "off the bottom edge"},
        {kCentre, 40, "too large for the panel"},
    };
    for (const Case& k : cases) {
        Canvas c;
        Canvas pristine;
        DrawOctopusMark(c.data(), kW, kH, k.where, k.scale);
        if (c.fb != pristine.fb) {
            std::printf("  (case: %s)\n", k.why);
            CHECK(false);
        } else {
            CHECK(true);
        }
    }
}

static void test_a_null_framebuffer_is_refused() {
    DrawOctopusMark(nullptr, kW, kH, kCentre, 2);
    CHECK(true);   // reaching here without a crash is the assertion
}

// -------------------------------------------------------------------- main --

int main() {
    std::printf("octopus_mark host tests (real firmware translation unit)\n\n");

    RUN(test_the_golden_is_the_declared_grid);
    RUN(test_the_mascot_matches_the_historical_sprite);
    RUN(test_scaling_is_a_block_fill);
    RUN(test_the_welcome_screen_draws_it_at_the_composer_scale);
    RUN(test_the_body_is_red_and_the_face_is_black);
    RUN(test_only_three_pigments_appear);
    RUN(test_no_weather_accessory_is_drawn);

    RUN(test_no_pixel_escapes_the_ink_bounds);
    RUN(test_the_ink_is_centred_and_the_sprite_contains_it);

    RUN(test_the_same_call_draws_the_same_bytes);
    RUN(test_refusals_draw_nothing);
    RUN(test_a_null_framebuffer_is_refused);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
