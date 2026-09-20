/**
 * @file test_welcome_screen.cc
 * @brief Host tests for main/rawdraw/welcome_screen.cc, and for the one thing
 *        about dashboard_renderer.cc a host can still decide.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * This is the suite that answers "could this screen ever show a secret", and it
 * answers it structurally rather than by inspection: the only input to the QR
 * is an IP address, the only characters that get through are digits and dots,
 * and the payload is assembled here rather than supplied. There is no
 * argument a caller could pass that produces a different URL, so there is no
 * path by which a pairing token reaches the glass — which matters because
 * e-paper keeps its last image with the power off.
 *
 * It is also the suite that answers "could this screen run off the panel
 * again". On hardware lot 1 it ran off the right-hand edge; on lot 3 it lost
 * its title off the top. Four things are pinned about that, and none of them
 * needs a font:
 *
 *   - every rectangle this screen declares is inside the safe box, and the
 *     right column does not overlap the QR beside it or the mascot below it;
 *   - nothing is declared, and no helper draws a pixel, above kWelcomeSafeTop —
 *     the band the lot-3 title disappeared into, refused now by a number rather
 *     than by an estimate of how deep the plastic is;
 *   - FitWelcomeLine() never returns something wider than the room it was
 *     given, at any width, for any candidate list — which is what turns "the
 *     copy happens to fit today" into "no copy can overflow";
 *   - the renderer's own strings are English and ASCII, and the product name is
 *     drawn through the right column's box, read back out of the source file
 *     because the words and the call site are the part a reader of the diff
 *     cannot otherwise check.
 *
 * What this suite still cannot see is where the ink of a line of text actually
 * lands. welcome_screen.h records what the lot-3 photograph measures about
 * that, and why this screen works around it rather than encoding it.
 *
 * The rest is what the panel would otherwise be wrong about and a reader could
 * not tell: the fallback occupies exactly the QR's box so the layout does not
 * move and no stale modules survive under new text, and the swatch row shows
 * four literal pigments in hardware order, unlabelled.
 *
 * The *drawing* of text is still not here. DrawText pulls in the CJK font
 * component and esp_heap_caps.h, neither of which this harness links.
 */

#include "rawdraw/welcome_screen.h"

#include "rawdraw/octopus_mark.h"
#include "rawdraw/qr_render.h"

#include <algorithm>
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
        std::printf("%-60s %s\n", #fn,                             \
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

static int CountOf(const Canvas& c, const Rect& r, Color want) {
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            if (c.at(x, y) == want) ++n;
        }
    }
    return n;
}

/**
 * @brief A font made of identical letters.
 *
 * FitWelcomeLine() takes a width function precisely so that this suite can
 * supply one. Ten pixels per Unicode code point — code points and not bytes, so
 * that a multi-byte string is measured the way a real font would measure it and
 * the truncation arithmetic is exercised honestly.
 */
static int RulerWidth(const char* text, void* ctx) {
    const int per = *static_cast<const int*>(ctx);
    int n = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) ++n;
    }
    return n * per;
}

static int g_per_char = 10;
static void* kRuler = &g_per_char;

/// Every byte sequence in @p s is a complete, well-formed UTF-8 character.
static bool IsWellFormedUtf8(const char* s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0) {
        int extra;
        if (*p < 0x80) extra = 0;
        else if ((*p & 0xE0) == 0xC0) extra = 1;
        else if ((*p & 0xF0) == 0xE0) extra = 2;
        else if ((*p & 0xF8) == 0xF0) extra = 3;
        else return false;
        ++p;
        for (int i = 0; i < extra; ++i, ++p) {
            if ((*p & 0xC0) != 0x80) return false;
        }
    }
    return true;
}

// -------------------------------------------------------------- the payload --

static void test_a_lan_address_becomes_the_device_own_url() {
    char url[64];
    CHECK(WelcomeQrUrl("192.168.1.42", url, sizeof(url)));
    CHECK(std::strcmp(url, "http://192.168.1.42/") == 0);

    CHECK(WelcomeQrUrl("10.0.0.1", url, sizeof(url)));
    CHECK(std::strcmp(url, "http://10.0.0.1/") == 0);
}

/**
 * NOTHING BUT AN ADDRESS GETS THROUGH.
 *
 * Each of these is a shape somebody would have to supply in order to make this
 * screen display something other than the device's own address — a path to hang
 * a token on, a query string, a second host, a scheme of their own. The
 * function takes an address, so all of them are refused at the door and the
 * refusal leaves nothing behind for the caller to draw by accident.
 */
static void test_anything_that_is_not_an_address_is_refused() {
    const char* const rejected[] = {
        "",
        "192.168.1.42/pair?token=deadbeef",
        "192.168.1.42 token",
        "evil.example.com",
        "192.168.1.42#tok",
        "2001:db8::1",
        "http://192.168.1.42/",      // already a URL: not this function's input
        "192.168.1.42/",
        "192.168.1.42\n",
        "192.168.1.42%00",
    };
    for (const char* bad : rejected) {
        char url[64];
        std::memset(url, 'X', sizeof(url));
        if (WelcomeQrUrl(bad, url, sizeof(url))) {
            std::printf("  (accepted: \"%s\")\n", bad);
            CHECK(false);
        } else {
            CHECK(url[0] == '\0');
        }
    }

    char url[64];
    CHECK(!WelcomeQrUrl(nullptr, url, sizeof(url)));
    CHECK(url[0] == '\0');
}

/// A buffer that cannot hold the result produces no result, rather than a
/// truncated URL that would encode to a scannable link to somewhere else.
static void test_a_short_buffer_refuses_rather_than_truncates() {
    char tiny[10];
    CHECK(!WelcomeQrUrl("192.168.1.42", tiny, sizeof(tiny)));
    CHECK(tiny[0] == '\0');
    CHECK(!WelcomeQrUrl("192.168.1.42", nullptr, 64));

    // Exactly big enough: "http://" (7) + "1.2.3.4" (7) + "/" (1) + NUL.
    char exact[16];
    CHECK(WelcomeQrUrl("1.2.3.4", exact, sizeof(exact)));
    CHECK(std::strcmp(exact, "http://1.2.3.4/") == 0);
}

// ------------------------------------------------------------ the QR states --

static void test_a_connected_device_draws_a_qr_in_its_box() {
    Canvas c;
    CHECK(DrawWelcomeQr(c.data(), kW, kH, "192.168.1.42"));

    const int black = CountOf(c, kWelcomeQrBox, BLACK);
    CHECK(black > 200);
    // Black and white only: RED and YELLOW collapse to black on the 1bpp panel
    // variant, which would destroy the contrast a scan depends on.
    CHECK(CountOf(c, kWelcomeQrBox, RED) == 0);
    CHECK(CountOf(c, kWelcomeQrBox, YELLOW) == 0);

    // And nothing outside the box moved: the detail column and the swatch row
    // are drawn by the caller and must not be overwritten.
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const bool inside =
                x >= kWelcomeQrBox.x && x < kWelcomeQrBox.x + kWelcomeQrBox.w &&
                y >= kWelcomeQrBox.y && y < kWelcomeQrBox.y + kWelcomeQrBox.h;
            if (!inside && c.at(x, y) != WHITE) {
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

/**
 * THE PAYLOAD AND THE QUIET ZONE ARE THE PART THAT MUST NOT MOVE.
 *
 * Everything else on this screen was redrawn after lot 3 — the copy, the
 * mascot, the column geometry. The code was not, and this pins that: the same
 * URL, at the same origin, with the same white margin around it, so a phone
 * that scanned the old panel scans the new one.
 */
static void test_the_qr_payload_and_quiet_zone_are_unchanged() {
    CHECK(kWelcomeQrBox.x == 20);
    // The title's move out of the top band did not touch this box. Its origin,
    // size, quiet zone, module size and payload are the part of this screen a
    // photograph proved working, and all five are where lot 3 left them.
    CHECK(kWelcomeQrBox.y == 62);
    CHECK(kWelcomeQrBox.w == 168);
    CHECK(kWelcomeQrBox.h == 168);

    Canvas direct;
    Canvas via_screen;
    char url[64];
    CHECK(WelcomeQrUrl("192.168.1.42", url, sizeof(url)));
    CHECK(std::strcmp(url, "http://192.168.1.42/") == 0);
    CHECK(DrawQrCode(direct.data(), kW, kH, kWelcomeQrBox, url));
    CHECK(DrawWelcomeQr(via_screen.data(), kW, kH, "192.168.1.42"));
    CHECK(direct.fb == via_screen.fb);

    // The quiet zone: the outermost ring of the box is white on all four sides.
    // Without it a scanner has no way to find the finder patterns.
    const Rect& b = kWelcomeQrBox;
    bool quiet = true;
    for (int x = b.x; x < b.x + b.w && quiet; ++x) {
        quiet = via_screen.at(x, b.y) == WHITE &&
                via_screen.at(x, b.y + b.h - 1) == WHITE;
    }
    for (int y = b.y; y < b.y + b.h && quiet; ++y) {
        quiet = via_screen.at(b.x, y) == WHITE &&
                via_screen.at(b.x + b.w - 1, y) == WHITE;
    }
    CHECK(quiet);
}

/// The no-address state: no QR at all, and nothing drawn. A QR encoding an
/// empty or invented address is worse than no QR, because it looks like it
/// should work.
static void test_no_address_draws_no_qr_at_all() {
    Canvas c;
    Canvas pristine;
    CHECK(!DrawWelcomeQr(c.data(), kW, kH, ""));
    CHECK(c.fb == pristine.fb);

    CHECK(!DrawWelcomeQr(c.data(), kW, kH, nullptr));
    CHECK(c.fb == pristine.fb);
}

/**
 * THE FALLBACK OCCUPIES THE QR'S BOX EXACTLY.
 *
 * Two things ride on this. The layout must not move between a connected and a
 * disconnected device, or the screen jumps every time Wi-Fi drops. And the box
 * must be *cleared*, not merely outlined: this renderer is called again over a
 * framebuffer that may still hold the previous render, and a "No Wi-Fi yet"
 * message sitting on top of a still-scannable QR for an address the device no
 * longer has is the worst outcome of the three.
 */
static void test_the_fallback_clears_and_outlines_the_same_box() {
    Canvas c;
    CHECK(DrawWelcomeQr(c.data(), kW, kH, "192.168.1.42"));
    CHECK(CountOf(c, kWelcomeQrBox, BLACK) > 200);

    DrawWelcomeQrFallbackBox(c.data(), kW, kH);

    // Only the border is left: the perimeter, and nothing inside it.
    const Rect inner{kWelcomeQrBox.x + 2, kWelcomeQrBox.y + 2,
                     kWelcomeQrBox.w - 4, kWelcomeQrBox.h - 4};
    CHECK(CountOf(c, inner, BLACK) == 0);
    CHECK(CountOf(c, inner, WHITE) == inner.w * inner.h);

    // And there really is an outline, so the box does not vanish.
    CHECK(c.at(kWelcomeQrBox.x, kWelcomeQrBox.y) == BLACK);
    CHECK(c.at(kWelcomeQrBox.x + kWelcomeQrBox.w - 1,
               kWelcomeQrBox.y + kWelcomeQrBox.h - 1) == BLACK);

    // Nothing outside the box.
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const bool inside =
                x >= kWelcomeQrBox.x && x < kWelcomeQrBox.x + kWelcomeQrBox.w &&
                y >= kWelcomeQrBox.y && y < kWelcomeQrBox.y + kWelcomeQrBox.h;
            if (!inside && c.at(x, y) != WHITE) {
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

/**
 * An IPv6 literal reaches the fallback rather than a QR.
 *
 * It cannot be written unbracketed into a URL, and bracketing it here would
 * produce an address nobody checked the device answers on. A code that resolves
 * to nothing is worse than a line of text saying why there is no code, because
 * the person holding the phone cannot tell which of the two they are looking
 * at until they have tried it.
 */
static void test_an_address_this_screen_cannot_encode_falls_back() {
    Canvas c;
    Canvas pristine;
    CHECK(!DrawWelcomeQr(c.data(), kW, kH,
                         "2001:0db8:85a3:0000:0000:8a2e:0370:7334"));
    CHECK(c.fb == pristine.fb);

    char url[64];
    CHECK(!WelcomeQrUrl("2001:0db8:85a3:0000:0000:8a2e:0370:7334", url, sizeof(url)));
    CHECK(url[0] == '\0');
}

/**
 * The QR's own scannability limit, reached through this screen's entry point.
 *
 * A dotted string that passes the address filter and is still far too long to
 * draw at three pixels per module must produce nothing rather than a square of
 * unresolvable noise. Not a realistic DHCP lease — the point is that the two
 * refusals compose, so a future address format that slipped past the filter
 * would still not put an unscannable code on the glass.
 */
static void test_an_address_too_long_for_a_scannable_code_falls_back() {
    const char* const absurd =
        "111.111.111.111.111.111.111.111.111.111.111.111.111.111"
        ".111.111.111.111.111.111.111.111.111.111.111.111.111";
    char url[128];
    CHECK(WelcomeQrUrl(absurd, url, sizeof(url)));

    Canvas c;
    Canvas pristine;
    CHECK(!DrawQrCode(c.data(), kW, kH, kWelcomeQrBox, url));
    CHECK(c.fb == pristine.fb);
}

// ------------------------------------------------------------ line fitting --

static void test_the_first_candidate_that_fits_whole_wins() {
    const char* const cands[] = {"Ready for a dashboard", "Ready to receive",
                                 "Ready"};
    char out[64];

    // 21 characters at 10 px each.
    CHECK(FitWelcomeLine(cands, 3, 210, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(std::strcmp(out, "Ready for a dashboard") == 0);

    // One pixel short of the first: the second is chosen whole, not the first
    // chopped. A truncated sentence says less than a shorter true one.
    CHECK(FitWelcomeLine(cands, 3, 209, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(std::strcmp(out, "Ready to receive") == 0);

    CHECK(FitWelcomeLine(cands, 3, 100, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(std::strcmp(out, "Ready") == 0);
}

/// The address is the one string nobody can write short, so it is the one that
/// gets an ellipsis. The QR beside it still carries the whole URL.
static void test_the_last_candidate_is_ellipsised_when_nothing_fits() {
    const char* const cands[] = {"http://192.168.100.200/", "192.168.100.200"};
    char out[64];

    CHECK(FitWelcomeLine(cands, 2, 230, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(std::strcmp(out, "http://192.168.100.200/") == 0);

    CHECK(FitWelcomeLine(cands, 2, 160, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(std::strcmp(out, "192.168.100.200") == 0);

    // Room for eight characters: five of the address and the three dots.
    CHECK(FitWelcomeLine(cands, 2, 80, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(std::strcmp(out, "192.1...") == 0);
}

/// Below that, nothing. A single character and an ellipsis is not a line of
/// text, it is a smear, and the screen reads better without it.
static void test_a_line_with_no_room_is_dropped_rather_than_drawn() {
    const char* const cands[] = {"192.168.100.200"};
    char out[64];

    CHECK(FitWelcomeLine(cands, 1, 40, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(std::strcmp(out, "1...") == 0);

    CHECK(!FitWelcomeLine(cands, 1, 30, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(out[0] == '\0');
    CHECK(!FitWelcomeLine(cands, 1, 0, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(out[0] == '\0');
    CHECK(!FitWelcomeLine(cands, 1, -5, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(out[0] == '\0');
}

/**
 * THE PROPERTY THE LOT 3 PANEL BROKE, SWEPT.
 *
 * Not "these strings fit at this width" — that is what somebody measured by eye
 * last time — but "for every candidate list this screen uses and every width
 * from nothing to the whole panel, what comes back is either nothing or
 * something that fits". Swept over the per-character width too, so the answer
 * does not depend on the ruler happening to match the font.
 */
static void test_nothing_wider_than_the_room_is_ever_returned() {
    const char* const a[] = {"Ready for a dashboard", "Ready to receive", "Ready"};
    const char* const b[] = {"http://192.168.100.200/", "192.168.100.200"};
    const char* const c[] = {"Network service off", "LAN service off", "LAN off"};
    const char* const d[] = {"Scan to connect", "Scan me"};
    const char* const e[] = {"NOTE4C"};
    struct List { const char* const* items; int n; };
    const List lists[] = {{a, 3}, {b, 2}, {c, 3}, {d, 2}, {e, 1}};

    bool ok = true;
    for (const int per : {6, 8, 10, 14}) {
        g_per_char = per;
        for (const List& l : lists) {
            for (int max_w = 0; max_w <= 400 && ok; ++max_w) {
                char out[64];
                const bool drew =
                    FitWelcomeLine(l.items, l.n, max_w, RulerWidth, kRuler, out,
                                   sizeof(out));
                if (!drew) {
                    if (out[0] != '\0') ok = false;
                    continue;
                }
                if (RulerWidth(out, kRuler) > max_w) {
                    std::printf("  (per %d, max %d: \"%s\" is %d wide)\n", per,
                                max_w, out, RulerWidth(out, kRuler));
                    ok = false;
                }
                if (!IsWellFormedUtf8(out)) ok = false;
            }
        }
    }
    g_per_char = 10;
    CHECK(ok);
}

/// A truncation point is never inside a character. The screen's own copy is
/// ASCII, so this failure would only ever appear in a translation — which is
/// precisely the case nobody would be running these tests for.
static void test_multibyte_text_is_never_cut_in_half() {
    // Six two-byte characters: é repeated.
    const char* const cands[] = {"\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"};
    char out[64];
    bool ok = true;
    for (int max_w = 0; max_w <= 120; ++max_w) {
        if (!FitWelcomeLine(cands, 1, max_w, RulerWidth, kRuler, out, sizeof(out))) {
            continue;
        }
        if (!IsWellFormedUtf8(out)) {
            std::printf("  (max %d produced a split character)\n", max_w);
            ok = false;
        }
        if (RulerWidth(out, kRuler) > max_w) ok = false;
    }
    CHECK(ok);
}

/// The output buffer is respected even when the width would allow more, and a
/// caller that passes nothing gets nothing rather than a crash.
static void test_the_output_buffer_and_the_null_arguments_are_respected() {
    const char* const cands[] = {"Ready for a dashboard", "Ready"};
    char small[8];
    CHECK(FitWelcomeLine(cands, 2, 4000, RulerWidth, kRuler, small, sizeof(small)));
    CHECK(std::strcmp(small, "Ready") == 0);

    char out[64];
    CHECK(!FitWelcomeLine(nullptr, 2, 200, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(out[0] == '\0');
    CHECK(!FitWelcomeLine(cands, 0, 200, RulerWidth, kRuler, out, sizeof(out)));
    CHECK(!FitWelcomeLine(cands, 2, 200, nullptr, kRuler, out, sizeof(out)));
    CHECK(!FitWelcomeLine(cands, 2, 200, RulerWidth, kRuler, nullptr, 64));
    CHECK(!FitWelcomeLine(cands, 2, 200, RulerWidth, kRuler, out, 0));
    CHECK(!FitWelcomeLine(cands, 2, 200, RulerWidth, kRuler, out, 3));
    CHECK(out[0] == '\0');
}

// ---------------------------------------------------------------- layout --

/// Every rectangle this screen declares, in one place, so the sweep below
/// cannot quietly stop covering one of them.
struct NamedRect { const char* name; Rect r; };

static std::vector<NamedRect> AllPrimitives() {
    std::vector<NamedRect> v = {
        {"title", kWelcomeTitleBox},
        {"qr box", kWelcomeQrBox},
        {"caption", kWelcomeCaptionBox},
        {"detail", kWelcomeDetailBox},
        {"octopus", OctopusBounds(kWelcomeOctopusCenter, kWelcomeOctopusScale)},
        {"swatch rule", Rect{kWelcomeSafeLeft, kWelcomeSwatchRuleY,
                             kWelcomeSafeRight - kWelcomeSafeLeft, 1}},
        {"swatch row", kWelcomeSwatchRow},
    };

    // The fallback's lines of text, which live inside the QR's box.
    for (int i = 0; i < kWelcomeQrTextLines; ++i) {
        v.push_back({"qr fallback line",
                     Rect{kWelcomeQrBox.x + kWelcomeQrTextInset,
                          kWelcomeQrBox.y + kWelcomeQrTextTop +
                              i * kWelcomeQrTextLineH,
                          kWelcomeQrBox.w - 2 * kWelcomeQrTextInset, 16}});
    }
    // And the detail column's, which are the ones that ran off the panel.
    for (int i = 0; i < kWelcomeDetailLines; ++i) {
        v.push_back({"detail line",
                     Rect{kWelcomeDetailBox.x,
                          kWelcomeDetailBox.y + i * kWelcomeDetailLineH,
                          kWelcomeDetailBox.w, 16}});
    }

    PaletteSwatch sw[kWelcomeSwatchCount];
    WelcomeSwatchLayout(sw);
    for (int i = 0; i < kWelcomeSwatchCount; ++i) {
        v.push_back({"swatch chip", sw[i].chip});
    }
    return v;
}

/**
 * THE TITLE IS IN THE RIGHT COLUMN, NOT IN THE BAND THAT ATE IT TWICE.
 *
 * This is the whole point of the change and it is decidable without a font: the
 * product name's box is the right column's box, it starts no higher than the y
 * that lot 3 photographed rendering a whole line of text, and it sits clear of
 * the two lines under it. `kWelcomeHeaderBox` and `kWelcomeHeaderRuleY` are
 * gone, so a title drawn back into the top band would not compile; what a test
 * can still add is that the column the title claims to be in is the column the
 * other lines are in.
 */
static void test_the_title_sits_in_the_visible_right_column() {
    // Same column as the facts under it, and its right edge is the safe edge.
    CHECK(kWelcomeTitleBox.x == kWelcomeDetailBox.x);
    CHECK(kWelcomeTitleBox.w == kWelcomeDetailBox.w);
    CHECK(kWelcomeTitleBox.x + kWelcomeTitleBox.w == kWelcomeSafeRight);
    // Clear of the QR to its left.
    CHECK(kWelcomeTitleBox.x > kWelcomeQrBox.x + kWelcomeQrBox.w);

    // 64 is not a margin, it is the y at which lot 3 drew `4-color e-paper` and
    // the photograph shows that line whole. Nothing on this screen is handed a
    // smaller one.
    CHECK(kWelcomeTitleBox.y >= 64);
    // Tall enough for the Medium face's 24 px line, which the old 20 px band
    // was not.
    CHECK(kWelcomeTitleBox.h >= 24);

    // And the lines below start after it rather than on top of it.
    CHECK(kWelcomeDetailBox.y >= kWelcomeTitleBox.y + kWelcomeTitleBox.h);
}

/**
 * NOTHING IS DRAWN IN THE BAND THIS SCREEN LOST A TITLE IN.
 *
 * The refusal, stated as a number and swept over every rectangle. 62 is the QR
 * box's top — the one primitive on the lot-3 panel that was photographed,
 * scanned and proved to land on visible glass. The 8 it replaces was a
 * self-assertion nobody had measured. The inequality is a floor and not an
 * equality on purpose: the band may be widened by a future photograph, it may
 * not be narrowed by an argument.
 */
static void test_the_band_that_lost_the_title_is_refused() {
    CHECK(kWelcomeSafeTop >= 62);
    CHECK(kWelcomeQrBox.y >= kWelcomeSafeTop);

    for (const NamedRect& n : AllPrimitives()) {
        if (n.r.y < kWelcomeSafeTop) {
            std::printf("  (%s starts at y=%d, above the refused band at %d)\n",
                        n.name, n.r.y, kWelcomeSafeTop);
            CHECK(false);
        }
    }
    CHECK(true);
}

/**
 * AND NOT ONE PIXEL EITHER.
 *
 * The rectangles above are what this screen *declares*; this is what it
 * *draws*. Every primitive a host can run — the QR, its fallback box, the
 * mascot and the swatch row — onto one canvas, and then the band above
 * kWelcomeSafeTop read back. A helper that quietly drew a border, a shadow or a
 * quiet zone one row higher than its rectangle says would pass the sweep and
 * fail here.
 *
 * The drawing of *text* is still not covered — DrawText needs the CJK font
 * component — which is why the rectangles are swept as well.
 */
static void test_the_refused_band_receives_no_ink() {
    Canvas c;
    DrawWelcomeQr(c.data(), kW, kH, "192.168.1.42");
    DrawWelcomeQrFallbackBox(c.data(), kW, kH);
    DrawWelcomeSwatches(c.data(), kW, kH);
    DrawOctopusMark(c.data(), kW, kH, kWelcomeOctopusCenter,
                    kWelcomeOctopusScale);

    for (int y = 0; y < kWelcomeSafeTop; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (c.at(x, y) != WHITE) {
                std::printf("  (ink at %d,%d, above the refused band at %d)\n",
                            x, y, kWelcomeSafeTop);
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);
}

/// The mascot is still big enough to read. At 2x it measured 58x52 on a 400x300
/// panel and was a smudge from arm's length on lot 3; at 4x the photograph
/// shows a legible octopus.
static void test_the_mascot_is_big_enough_to_read() {
    CHECK(kWelcomeOctopusScale >= 4);
}

/**
 * NOTHING CROSSES x = 380, AND NOTHING LEAVES THE SAFE BOX.
 *
 * The photograph of lot 3 shows three lines of text disappearing into the
 * right-hand edge of the glass. x:[20, 380] also clears the panel's physical
 * corner dead-zones at every y, because x never enters the excluded band.
 */
static void test_every_primitive_is_inside_the_safe_box() {
    for (const NamedRect& n : AllPrimitives()) {
        const bool ok = n.r.x >= kWelcomeSafeLeft &&
                        n.r.x + n.r.w <= kWelcomeSafeRight &&
                        n.r.y >= kWelcomeSafeTop &&
                        n.r.y + n.r.h <= kWelcomeSafeBottom;
        if (!ok) {
            std::printf("  (%s: %d,%d %dx%d)\n", n.name, n.r.x, n.r.y, n.r.w,
                        n.r.h);
        }
        CHECK(ok);
    }
}

/**
 * THE RIGHT COLUMN CLEARS WHAT IS AROUND IT.
 *
 * "Inside the panel" is not enough on its own: text that overlapped the QR
 * would be just as broken and would still pass the sweep above. The column
 * starts to the right of the QR's box, the mascot sits below the last line of
 * it, and the rule above the swatches sits below the mascot.
 */
static void test_the_right_column_overlaps_nothing() {
    const Rect octo = OctopusBounds(kWelcomeOctopusCenter, kWelcomeOctopusScale);

    CHECK(kWelcomeDetailBox.x > kWelcomeQrBox.x + kWelcomeQrBox.w);
    CHECK(kWelcomeDetailBox.x + kWelcomeDetailBox.w == kWelcomeSafeRight);
    CHECK(kWelcomeDetailBox.y >= kWelcomeSafeTop);

    CHECK(octo.y >= kWelcomeDetailBox.y + kWelcomeDetailBox.h);
    CHECK(octo.y + octo.h <= kWelcomeSwatchRuleY);
    CHECK(octo.x > kWelcomeQrBox.x + kWelcomeQrBox.w);

    // The mascot is centred in the column it lives in, to within the odd pixel.
    const Rect ink = OctopusInkBounds(kWelcomeOctopusCenter, kWelcomeOctopusScale);
    const int column_mid = (kWelcomeDetailBox.x + kWelcomeSafeRight) / 2;
    CHECK(ink.x + ink.w / 2 == column_mid);

    // The caption sits under the QR and above the rule, not across either.
    CHECK(kWelcomeCaptionBox.y >= kWelcomeQrBox.y + kWelcomeQrBox.h);
    CHECK(kWelcomeCaptionBox.y + kWelcomeCaptionBox.h <= kWelcomeSwatchRuleY);
}

// --------------------------------------------------------- the swatch row --

/**
 * FOUR LITERAL PIGMENTS, IN HARDWARE ORDER, AS THEMSELVES.
 *
 * Read back from the framebuffer rather than from the layout struct, because
 * the failure this guards against is somebody routing the row through
 * ThemeManager or DrawDitherRect — both of which would leave the layout
 * identical and the picture a lie about what this panel can show.
 */
static void test_the_four_pigments_are_drawn_as_themselves() {
    Canvas c;
    DrawWelcomeSwatches(c.data(), kW, kH);

    PaletteSwatch sw[kWelcomeSwatchCount];
    WelcomeSwatchLayout(sw);

    const Color expected[kWelcomeSwatchCount] = {WHITE, BLACK, YELLOW, RED};
    for (int i = 0; i < kWelcomeSwatchCount; ++i) {
        CHECK(sw[i].color == expected[i]);

        // The middle of the chip, well inside any border.
        const int cx = sw[i].chip.x + sw[i].chip.w / 2;
        const int cy = sw[i].chip.y + sw[i].chip.h / 2;
        CHECK(c.at(cx, cy) == expected[i]);

        // Solid, not dithered or striped: every interior pixel is the same
        // colour. A halftone would pass a single-pixel probe.
        const Rect interior{sw[i].chip.x + 3, sw[i].chip.y + 3, sw[i].chip.w - 6,
                            sw[i].chip.h - 6};
        CHECK(CountOf(c, interior, expected[i]) == interior.w * interior.h);

        // Bordered, so each chip reads as a chip.
        CHECK(c.at(sw[i].chip.x, sw[i].chip.y) == BLACK);
    }
}

/**
 * THE CHIPS ARE UNLABELLED, AND THE ROW DRAWS NOTHING BUT CHIPS.
 *
 * Lot 3 printed "Blanc / Noir / Jaune / Rouge" across the squares themselves.
 * The words are gone and so is the field that carried them, so the only way
 * this could come back is a caller drawing its own text — which is what the
 * source scan below covers. Here: every pixel in the row's band that is not in
 * a chip is untouched, and so is the strip below it where the labels sat.
 */
static void test_the_row_draws_chips_and_nothing_else() {
    Canvas c;
    DrawWelcomeSwatches(c.data(), kW, kH);

    PaletteSwatch sw[kWelcomeSwatchCount];
    WelcomeSwatchLayout(sw);

    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            bool in_chip = false;
            for (int i = 0; i < kWelcomeSwatchCount && !in_chip; ++i) {
                in_chip = x >= sw[i].chip.x && x < sw[i].chip.x + sw[i].chip.w &&
                          y >= sw[i].chip.y && y < sw[i].chip.y + sw[i].chip.h;
            }
            if (!in_chip && c.at(x, y) != WHITE) {
                std::printf("  (ink outside every chip at %d,%d)\n", x, y);
                CHECK(false);
                return;
            }
        }
    }
    CHECK(true);

    // The band the labels used to occupy is inside the panel and empty, which
    // is why the row did not have to move when they went.
    CHECK(kWelcomeSwatchRow.y + kWelcomeSwatchRow.h <= kWelcomeSafeBottom);
    CHECK(CountOf(c, Rect{kWelcomeSafeLeft,
                          kWelcomeSwatchRow.y + kWelcomeSwatchRow.h,
                          kWelcomeSafeRight - kWelcomeSafeLeft,
                          kWelcomeSafeBottom - kWelcomeSwatchRow.y -
                              kWelcomeSwatchRow.h},
                  WHITE) > 0);
}

/// WHITE is the same colour as the canvas, so its border is what makes it
/// visible at all. Two pixels rather than one, deliberately.
static void test_the_white_swatch_has_the_thicker_border() {
    PaletteSwatch sw[kWelcomeSwatchCount];
    WelcomeSwatchLayout(sw);
    CHECK(sw[0].color == WHITE);
    CHECK(sw[0].border_px == 2);
    for (int i = 1; i < kWelcomeSwatchCount; ++i) {
        CHECK(sw[i].border_px == 1);
    }

    Canvas c;
    DrawWelcomeSwatches(c.data(), kW, kH);
    // Both border pixels of the white chip are black; the pixel after them is
    // the white fill, which is what makes the chip readable as a square.
    CHECK(c.at(sw[0].chip.x, sw[0].chip.y + sw[0].chip.h / 2) == BLACK);
    CHECK(c.at(sw[0].chip.x + 1, sw[0].chip.y + sw[0].chip.h / 2) == BLACK);
    CHECK(c.at(sw[0].chip.x + 2, sw[0].chip.y + sw[0].chip.h / 2) == WHITE);
}

static void test_the_swatches_are_evenly_spaced_inside_the_row() {
    PaletteSwatch sw[kWelcomeSwatchCount];
    WelcomeSwatchLayout(sw);

    const int cell_w = kWelcomeSwatchRow.w / kWelcomeSwatchCount;
    for (int i = 0; i < kWelcomeSwatchCount; ++i) {
        CHECK(sw[i].cell_w == cell_w);
        CHECK(sw[i].cell_x == kWelcomeSwatchRow.x + i * cell_w);
        // Centred in its cell.
        CHECK(sw[i].chip.x + sw[i].chip.w / 2 == sw[i].cell_x + cell_w / 2);
        // Exactly the four 28x28 squares the hardware note asks for.
        CHECK(sw[i].chip.w == 28);
        CHECK(sw[i].chip.h == 28);
    }
}

// ------------------------------------------------- the renderer's own copy --

/**
 * @brief Every double-quoted literal in a source file, minus the comments.
 *
 * Crude on purpose. It skips whole lines that begin a comment or a preprocessor
 * directive, which is enough for this one file and is much easier to be sure
 * about than a real lexer would be. A literal it misses is a check not
 * performed, never a false failure.
 */
static std::vector<std::string> StringLiteralsOf(const char* path, bool* found) {
    std::vector<std::string> out;
    *found = false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return out;
    *found = true;

    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);

    size_t line_start = 0;
    while (line_start <= all.size()) {
        size_t line_end = all.find('\n', line_start);
        if (line_end == std::string::npos) line_end = all.size();
        std::string line = all.substr(line_start, line_end - line_start);
        line_start = line_end + 1;

        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const char c0 = line[first];
        if (c0 == '*' || c0 == '#') continue;
        if (c0 == '/' && first + 1 < line.size() &&
            (line[first + 1] == '/' || line[first + 1] == '*')) {
            continue;
        }

        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '"') continue;
            std::string lit;
            ++i;
            for (; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                else lit.push_back(line[i]);
            }
            out.push_back(lit);
        }
    }
    return out;
}

static std::string Lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

/**
 * NOT ONE FRENCH WORD REACHES THE GLASS.
 *
 * The lot 3 panel read "Ecran e-paper 4 couleurs / Adresse : http://192.168…"
 * and labelled its swatches "Blanc Noir Jaune Rouge". The device's own
 * vocabulary is English everywhere else — nav::StatusLine(), the logs, the HTTP
 * API — and the slim Source Han Sans subset has no accented Latin, so French
 * was also the one language this panel could not spell correctly.
 *
 * Matched on whole words, so "connect" is not mistaken for "connecte" and
 * "important" is not mistaken for "port". Read out of the source file because
 * the renderer cannot be linked here: this is the only place a host can decide
 * what the panel says.
 */
static void test_the_renderer_draws_no_french() {
    static const char* const kFrench[] = {
        "ecran", "couleur", "couleurs", "adresse", "pret", "prete", "scannez",
        "connecte", "connectee", "reseau", "tableau", "maintenez", "blanc",
        "noir", "jaune", "rouge", "attente", "poulailler", "desactive",
        "lisible", "longue", "contre", "configurer", "pour", "les", "des",
        "une", "avec", "sans", "trop", "voir", "votre", "vous", "cette",
    };

    bool found = false;
    const std::vector<std::string> lits =
        StringLiteralsOf("main/ui/renderers/rawdraw/dashboard_renderer.cc", &found);
    CHECK(found);   // run from the firmware root; see tests/host/run.sh
    CHECK(lits.size() > 10);

    bool clean = true;
    for (const std::string& lit : lits) {
        const std::string low = Lower(lit);
        std::string word;
        for (size_t i = 0; i <= low.size(); ++i) {
            const char c = (i < low.size()) ? low[i] : ' ';
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                word.push_back(c);
                continue;
            }
            for (const char* fr : kFrench) {
                if (word == fr) {
                    std::printf("  (French word \"%s\" in \"%s\")\n", fr,
                                lit.c_str());
                    clean = false;
                }
            }
            word.clear();
        }
    }
    CHECK(clean);
}

/// And every one of them is plain ASCII. The font's Latin coverage is
/// U+0020..U+007E plus a few dozen symbols, so anything above that renders as a
/// hole — which is how an accent gets onto a panel without anybody noticing.
static void test_the_renderer_copy_is_plain_ascii() {
    bool found = false;
    const std::vector<std::string> lits =
        StringLiteralsOf("main/ui/renderers/rawdraw/dashboard_renderer.cc", &found);
    CHECK(found);

    bool ascii = true;
    for (const std::string& lit : lits) {
        for (const char c : lit) {
            if (static_cast<unsigned char>(c) > 0x7E ||
                (static_cast<unsigned char>(c) < 0x20 && c != '\n')) {
                std::printf("  (non-ASCII byte in \"%s\")\n", lit.c_str());
                ascii = false;
                break;
            }
        }
    }
    CHECK(ascii);
}

/// The whole file, comments included. Used only where the thing being checked
/// is the *absence* of an identifier, which a literal scan cannot see.
static std::string SourceOf(const char* path, bool* found) {
    std::string all;
    *found = false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return all;
    *found = true;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    return all;
}

/**
 * THE TITLE IS DRAWN THROUGH THE RIGHT COLUMN'S BOX, AND THE BANNER IS GONE.
 *
 * The geometry test above pins where kWelcomeTitleBox *is*; this pins that the
 * renderer draws the name into it. The two constants that used to put `NOTE4C`
 * in the top band no longer exist, so their absence here is belt and braces
 * against somebody reintroducing them with the same names and the same numbers
 * — but the rule that cannot be checked by compilation is that the one line
 * drawn in the Medium face is the one that goes through the title's box.
 *
 * Read out of the source because the renderer cannot be linked in this harness.
 */
static void test_the_renderer_draws_the_title_in_the_right_column() {
    static const char* const kPath =
        "main/ui/renderers/rawdraw/dashboard_renderer.cc";
    bool found = false;
    const std::string src = SourceOf(kPath, &found);
    CHECK(found);   // run from the firmware root; see tests/host/run.sh

    CHECK(src.find("kWelcomeTitleBox") != std::string::npos);
    CHECK(src.find("kWelcomeHeaderBox") == std::string::npos);
    CHECK(src.find("kWelcomeHeaderRuleY") == std::string::npos);

    // `NOTE4C` and the title's box are named within a few lines of each other,
    // which is the most a scan of this kind can honestly claim: the name is
    // drawn by the call that uses the box, not by some other call.
    const size_t name = src.find("\"NOTE4C\"");
    const size_t box = src.find("kWelcomeTitleBox");
    CHECK(name != std::string::npos);
    if (name != std::string::npos && box != std::string::npos) {
        CHECK(box > name);
        CHECK(std::count(src.begin() + static_cast<long>(name),
                         src.begin() + static_cast<long>(box), '\n') <= 2);
    }

    // The old rule under the title went with the band: the only horizontal rule
    // left on this screen is the one above the swatches.
    CHECK(src.find("kWelcomeSwatchRuleY") != std::string::npos);
    size_t rules = 0;
    for (size_t i = src.find("DrawHLine"); i != std::string::npos;
         i = src.find("DrawHLine", i + 1)) {
        ++rules;
    }
    CHECK(rules == 1);
}

/**
 * THE COPY DOES NOT COUNT THE PIGMENTS FOR THE READER.
 *
 * `4-color e-paper` was the line the title displaced, and it is the one this
 * screen can afford to lose: the swatch row shows the four pigments as
 * themselves, twenty rows further down the same panel. A line of text asserting
 * what a picture already shows is the kind of copy that comes back, so its
 * absence is pinned rather than assumed.
 */
static void test_the_copy_does_not_describe_the_swatch_row() {
    bool found = false;
    const std::vector<std::string> lits =
        StringLiteralsOf("main/ui/renderers/rawdraw/dashboard_renderer.cc", &found);
    CHECK(found);

    bool clean = true;
    for (const std::string& lit : lits) {
        const std::string low = Lower(lit);
        if (low.find("4-color") != std::string::npos ||
            low.find("4 color") != std::string::npos ||
            low.find("four color") != std::string::npos ||
            low.find("four-color") != std::string::npos) {
            std::printf("  (the row is shown, not described: \"%s\")\n",
                        lit.c_str());
            clean = false;
        }
    }
    CHECK(clean);
}

// ------------------------------------------------------------- behaviour --

/// Nothing crashes, and nothing is drawn, when the device could not allocate a
/// framebuffer. Reached on a board short of PSRAM, which the renderer tolerates
/// elsewhere too.
static void test_a_null_framebuffer_is_refused_everywhere() {
    CHECK(!DrawWelcomeQr(nullptr, kW, kH, "192.168.1.42"));
    DrawWelcomeQrFallbackBox(nullptr, kW, kH);
    DrawWelcomeSwatches(nullptr, kW, kH);
    WelcomeSwatchLayout(nullptr);
    CHECK(true);
}

/// The dedup in front of a 25-second refresh compares bytes: two renders of the
/// same state must be identical, or a device with no frame repaints on every
/// wake for ever.
static void test_the_same_state_draws_the_same_bytes() {
    Canvas a;
    Canvas b;
    DrawWelcomeQr(a.data(), kW, kH, "192.168.1.42");
    DrawWelcomeSwatches(a.data(), kW, kH);
    DrawOctopusMark(a.data(), kW, kH, kWelcomeOctopusCenter, kWelcomeOctopusScale);
    DrawWelcomeQr(b.data(), kW, kH, "192.168.1.42");
    DrawWelcomeSwatches(b.data(), kW, kH);
    DrawOctopusMark(b.data(), kW, kH, kWelcomeOctopusCenter, kWelcomeOctopusScale);
    CHECK(a.fb == b.fb);

    // And the same decisions: the fitter is a pure function of its inputs too.
    const char* const cands[] = {"Ready for a dashboard", "Ready"};
    char first[64];
    char again[64];
    CHECK(FitWelcomeLine(cands, 2, 150, RulerWidth, kRuler, first, sizeof(first)));
    CHECK(FitWelcomeLine(cands, 2, 150, RulerWidth, kRuler, again, sizeof(again)));
    CHECK(std::strcmp(first, again) == 0);
}

// -------------------------------------------------------------------- main --

int main() {
    std::printf("welcome_screen host tests (real firmware translation units)\n\n");

    RUN(test_a_lan_address_becomes_the_device_own_url);
    RUN(test_anything_that_is_not_an_address_is_refused);
    RUN(test_a_short_buffer_refuses_rather_than_truncates);

    RUN(test_a_connected_device_draws_a_qr_in_its_box);
    RUN(test_the_qr_payload_and_quiet_zone_are_unchanged);
    RUN(test_no_address_draws_no_qr_at_all);
    RUN(test_the_fallback_clears_and_outlines_the_same_box);
    RUN(test_an_address_this_screen_cannot_encode_falls_back);
    RUN(test_an_address_too_long_for_a_scannable_code_falls_back);

    RUN(test_the_first_candidate_that_fits_whole_wins);
    RUN(test_the_last_candidate_is_ellipsised_when_nothing_fits);
    RUN(test_a_line_with_no_room_is_dropped_rather_than_drawn);
    RUN(test_nothing_wider_than_the_room_is_ever_returned);
    RUN(test_multibyte_text_is_never_cut_in_half);
    RUN(test_the_output_buffer_and_the_null_arguments_are_respected);

    RUN(test_the_title_sits_in_the_visible_right_column);
    RUN(test_the_band_that_lost_the_title_is_refused);
    RUN(test_the_refused_band_receives_no_ink);
    RUN(test_the_mascot_is_big_enough_to_read);
    RUN(test_every_primitive_is_inside_the_safe_box);
    RUN(test_the_right_column_overlaps_nothing);

    RUN(test_the_four_pigments_are_drawn_as_themselves);
    RUN(test_the_row_draws_chips_and_nothing_else);
    RUN(test_the_white_swatch_has_the_thicker_border);
    RUN(test_the_swatches_are_evenly_spaced_inside_the_row);

    RUN(test_the_renderer_draws_no_french);
    RUN(test_the_renderer_copy_is_plain_ascii);
    RUN(test_the_renderer_draws_the_title_in_the_right_column);
    RUN(test_the_copy_does_not_describe_the_swatch_row);

    RUN(test_a_null_framebuffer_is_refused_everywhere);
    RUN(test_the_same_state_draws_the_same_bytes);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
