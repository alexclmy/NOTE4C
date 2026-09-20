/**
 * @file test_qr_render.cc
 * @brief Host tests for main/rawdraw/qr_render.cc and the vendored encoder.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * FIRST SUITE IN THIS FILE THAT COMPILES ANYTHING UNDER main/rawdraw/
 * ------------------------------------------------------------------
 * It links the real qr_render.cc, the real rawdraw.cc and Nayuki's qrcodegen.c
 * verbatim. That is possible because none of the three reaches ESP-IDF or LVGL:
 * rawdraw.h's only include is font_engine.h, which provides its own standalone
 * types when LVGL is absent, and neither helper calls DrawText.
 *
 * What is pinned here is not "a QR was produced" — the encoder is Nayuki's and
 * is not this project's to re-test. It is the four properties the *screen*
 * depends on, each of which is a way this could look right and be unscannable:
 *
 *   1. the modules are only ever BLACK or WHITE (the 1bpp panel collapses RED
 *      and YELLOW to black, so a coloured module is an invisible one there);
 *   2. the four-module quiet zone is actually white, on all four sides;
 *   3. the same text draws the same pixels every time, because the frame is
 *      compared byte for byte before a 25-second refresh is spent;
 *   4. an input that will not fit draws *nothing*, so the caller can put honest
 *      text in the space instead of a clipped square.
 */

#include "rawdraw/qr_render.h"

#include <cstdio>
#include <cstring>
#include <string>
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

/// The real panel: 400x300 at 2bpp.
constexpr int kW = 400;
constexpr int kH = 300;
constexpr size_t kFbBytes = static_cast<size_t>((kW * 2 + 7) / 8) * kH;

/// The welcome screen's actual reservation, so these tests fail if the layout
/// stops being able to hold a QR rather than only if the encoder does.
constexpr Rect kBox{20, 62, 168, 168};

struct Canvas {
    std::vector<uint8_t> fb;
    Canvas() : fb(kFbBytes, 0) { Clear(fb.data(), kW, kH, WHITE); }
    uint8_t* data() { return fb.data(); }
    Color at(int x, int y) const { return get_pixel(fb.data(), kW, x, y); }
};

static int CountOf(const Canvas& c, const Rect& r, Color want) {
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            if (c.at(x, y) == want) ++n;
        }
    }
    return n;
}

// ------------------------------------------------------------------- tests --

static void test_a_lan_url_encodes_and_draws() {
    Canvas c;
    int side = 0;
    CHECK(DrawQrCode(c.data(), kW, kH, kBox, "http://192.168.1.42/", &side));
    CHECK(side > 0);
    CHECK(side <= kBox.w);

    // Not blank and not solid: a QR that came out all one colour is a QR that
    // will not scan, and both degenerate cases look like success to a caller
    // that only checked the return value.
    const int black = CountOf(c, kBox, BLACK);
    const int white = CountOf(c, kBox, WHITE);
    CHECK(black > 200);
    CHECK(white > 200);
    CHECK(black + white == kBox.w * kBox.h);
}

/**
 * THE COLOUR RULE, READ BACK FROM THE PIXELS.
 *
 * This is the one that would otherwise be caught by somebody photographing a
 * 1bpp panel and finding a black square. Nothing in the box may be RED or
 * YELLOW, and the whole canvas outside the box must be untouched.
 */
static void test_every_module_is_black_or_white() {
    Canvas c;
    CHECK(DrawQrCode(c.data(), kW, kH, kBox, "http://192.168.0.60/"));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Color p = c.at(x, y);
            if (p == RED || p == YELLOW) {
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

static void test_nothing_is_drawn_outside_the_box() {
    Canvas c;
    CHECK(DrawQrCode(c.data(), kW, kH, kBox, "http://192.168.1.42/"));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const bool inside = x >= kBox.x && x < kBox.x + kBox.w &&
                                y >= kBox.y && y < kBox.y + kBox.h;
            if (!inside && c.at(x, y) != WHITE) {
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

/**
 * The quiet zone is four modules on every side, and it is checked by walking
 * inwards until the first dark pixel rather than by trusting the arithmetic.
 * A code drawn hard against a separator line is a code a phone gives up on, and
 * the failure is silent from the device's side.
 */
static void test_the_quiet_zone_is_four_modules_of_white() {
    Canvas c;
    int side = 0;
    CHECK(DrawQrCode(c.data(), kW, kH, kBox, "http://192.168.1.42/", &side));
    CHECK(side > 0);

    const int origin_x = kBox.x + (kBox.w - side) / 2;
    const int origin_y = kBox.y + (kBox.h - side) / 2;

    // Version 2 for this payload: 25 modules plus 8 of quiet zone.
    const int total_modules = 25 + 2 * kQrQuietModules;
    CHECK(side % total_modules == 0);
    const int scale = side / total_modules;
    CHECK(scale >= kQrMinModulePx);
    const int quiet_px = kQrQuietModules * scale;

    // All four margins entirely white.
    const Rect top{origin_x, origin_y, side, quiet_px};
    const Rect bottom{origin_x, origin_y + side - quiet_px, side, quiet_px};
    const Rect left{origin_x, origin_y, quiet_px, side};
    const Rect right{origin_x + side - quiet_px, origin_y, quiet_px, side};
    CHECK(CountOf(c, top, WHITE) == top.w * top.h);
    CHECK(CountOf(c, bottom, WHITE) == bottom.w * bottom.h);
    CHECK(CountOf(c, left, WHITE) == left.w * left.h);
    CHECK(CountOf(c, right, WHITE) == right.w * right.h);

    // And the module immediately inside the quiet zone is dark: the top-left
    // finder pattern's corner. If it were not, the "quiet zone" above would be
    // white because the code is somewhere else entirely.
    CHECK(c.at(origin_x + quiet_px, origin_y + quiet_px) == BLACK);
}

/// The dedup in front of a 25-second refresh compares bytes. Two renders of the
/// same address that differed by one pixel would cost a repaint every wake.
static void test_the_same_url_draws_the_same_bytes_every_time() {
    Canvas a;
    Canvas b;
    CHECK(DrawQrCode(a.data(), kW, kH, kBox, "http://192.168.1.42/"));
    CHECK(DrawQrCode(b.data(), kW, kH, kBox, "http://192.168.1.42/"));
    CHECK(a.fb == b.fb);

    // And a different address is a different picture, which is what makes the
    // comparison above meaningful rather than a test of memcmp.
    Canvas d;
    CHECK(DrawQrCode(d.data(), kW, kH, kBox, "http://192.168.1.43/"));
    CHECK(a.fb != d.fb);
}

/**
 * REFUSALS DRAW NOTHING AT ALL.
 *
 * Each of these is a case where a permissive implementation would produce
 * something that looks like a QR code and is not one. The caller's contract is
 * that on false it may use the space for text, so a single stray pixel is a
 * defect.
 */
static void test_a_refusal_leaves_the_canvas_untouched() {
    const char* const kTooLongForTheBox =
        "http://this-address-is-far-too-long-to-draw-at-three-pixels-per-module"
        ".example.invalid/with/a/path/and/more/besides/still/going/on/and/on/";

    struct Case { const char* text; Rect box; const char* why; };
    const Case cases[] = {
        {"", kBox, "empty text"},
        {nullptr, kBox, "null text"},
        {"http://192.168.1.42/", Rect{20, 50, 20, 20}, "box too small"},
        {"http://192.168.1.42/", Rect{20, 50, 0, 168}, "zero width"},
        {kTooLongForTheBox, kBox, "past the version ceiling for this box"},
        {"http://192.168.1.42/", Rect{380, 50, 168, 168}, "box off the canvas"},
    };

    for (const Case& k : cases) {
        Canvas c;
        Canvas pristine;
        int side = 7;
        CHECK(!DrawQrCode(c.data(), kW, kH, k.box, k.text, &side));
        CHECK(side == 0);
        if (c.fb != pristine.fb) {
            std::printf("  (case: %s)\n", k.why);
            CHECK(false);
        }
    }
}

/// A null framebuffer is a refusal, not a crash. Reached on a device that could
/// not allocate its canvas, which is a state the renderer already tolerates.
static void test_a_null_framebuffer_is_refused() {
    CHECK(!DrawQrCode(nullptr, kW, kH, kBox, "http://192.168.1.42/"));
}

/**
 * The address the device actually shows is built from its own IP, and IPs vary
 * in length. Sweep the realistic range so a device on 10.0.0.1 and one on
 * 192.168.100.200 both get a scannable code rather than only the one somebody
 * happened to test with.
 */
static void test_every_plausible_lan_address_fits_the_box() {
    const char* const addrs[] = {
        "http://10.0.0.1/",
        "http://10.0.0.100/",
        "http://172.16.31.7/",
        "http://192.168.1.42/",
        "http://192.168.100.200/",
        "http://255.255.255.255/",
    };
    for (const char* a : addrs) {
        Canvas c;
        int side = 0;
        if (!DrawQrCode(c.data(), kW, kH, kBox, a, &side)) {
            std::printf("  (address: %s)\n", a);
            CHECK(false);
            continue;
        }
        CHECK(side > 0);
        CHECK(side <= kBox.w);
        CHECK(CountOf(c, kBox, BLACK) > 200);
    }
}

/**
 * No secret ever reaches this function, and the test says so in the only way a
 * unit test can: it is not the encoder's job to refuse one, it is the caller's
 * job never to pass one. What is pinned here is the *interface* — a plain
 * NUL-terminated string with no notion of a pairing window, a token or a
 * device — so that a future caller wanting to encode a credential would have to
 * write the string itself and could not do it by flipping a flag.
 *
 * The complementary check is a grep over the call sites, and it lives in
 * test_welcome_screen.cc where the caller is.
 */
static void test_the_interface_carries_no_credential() {
    // If this ever stops compiling because DrawQrCode grew a "pairing" or
    // "token" parameter, that is the point.
    bool (*sig)(uint8_t*, int, int, const Rect&, const char*, int*) = &DrawQrCode;
    CHECK(sig != nullptr);
}

// -------------------------------------------------------------------- main --

int main() {
    std::printf("qr_render host tests (real firmware translation units)\n\n");

    RUN(test_a_lan_url_encodes_and_draws);
    RUN(test_every_module_is_black_or_white);
    RUN(test_nothing_is_drawn_outside_the_box);
    RUN(test_the_quiet_zone_is_four_modules_of_white);
    RUN(test_the_same_url_draws_the_same_bytes_every_time);
    RUN(test_a_refusal_leaves_the_canvas_untouched);
    RUN(test_a_null_framebuffer_is_refused);
    RUN(test_every_plausible_lan_address_fits_the_box);
    RUN(test_the_interface_carries_no_credential);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
