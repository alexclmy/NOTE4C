/**
 * @file welcome_screen.cc
 * @brief Implementation of the welcome screen's graphics. See the header.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "welcome_screen.h"

#include <stdio.h>
#include <string.h>

#include "qr_render.h"

namespace rawdraw {

namespace {

/**
 * @brief Digits and dots. That is the whole alphabet.
 *
 * Deliberately not "printable ASCII", and deliberately not hex digits and
 * colons either. Two reasons, and the second is the one that decided it:
 *
 *  - Nothing that could turn this into a different URL gets through. No slash
 *    to hang a path on, no `?`, no `@`, no letters to spell another host with.
 *    An IPv4 dotted quad needs exactly these eleven characters.
 *  - An IPv6 literal cannot be written into a URL unbracketed, and bracketing
 *    it here would produce an address this screen has no way to verify the
 *    device is actually reachable at. `WifiManager::GetIpAddress()` returns a
 *    dotted quad; anything else reaches the honest fallback box instead of a
 *    QR that resolves to nothing.
 */
bool IsAddressChar(char c) {
    return (c >= '0' && c <= '9') || c == '.';
}

constexpr int kSwatchChip = 28;

/// True for the trailing bytes of a UTF-8 sequence, which a truncation point
/// must never land on.
bool IsUtf8Continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

}  // namespace

bool WelcomeQrUrl(const char* ip, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return false;
    out[0] = '\0';
    if (ip == nullptr || ip[0] == '\0') return false;

    size_t len = 0;
    for (const char* p = ip; *p != '\0'; ++p, ++len) {
        if (!IsAddressChar(*p)) return false;
    }
    // "http://" + ip + "/" + NUL.
    if (7 + len + 1 + 1 > cap) return false;

    const int n = snprintf(out, cap, "http://%s/", ip);
    if (n <= 0 || static_cast<size_t>(n) >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool DrawWelcomeQr(uint8_t* fb, int width, int height, const char* ip) {
    char url[64];
    if (!WelcomeQrUrl(ip, url, sizeof(url))) return false;
    return DrawQrCode(fb, width, height, kWelcomeQrBox, url);
}

void DrawWelcomeQrFallbackBox(uint8_t* fb, int width, int height) {
    if (fb == nullptr) return;
    SetFramebufferHeightHint(height);
    // Cleared first. This box is the one place on the screen whose contents
    // change between states, and a fallback drawn over the top of a previous
    // render's QR modules would be the worst of both.
    DrawRect(fb, width, kWelcomeQrBox, WHITE);
    DrawRectBorder(fb, width, kWelcomeQrBox, 1, BLACK);
}

bool FitWelcomeLine(const char* const* candidates, int count, int max_width,
                    TextWidthFn measure, void* ctx, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return false;
    out[0] = '\0';
    if (candidates == nullptr || count <= 0 || measure == nullptr) return false;
    if (max_width <= 0) return false;

    // Longest first: the first one that fits whole is the most informative
    // wording this column has room for.
    for (int i = 0; i < count; ++i) {
        const char* c = candidates[i];
        if (c == nullptr || c[0] == '\0') continue;
        const size_t len = strlen(c);
        if (len + 1 > cap) continue;
        if (measure(c, ctx) <= max_width) {
            memcpy(out, c, len + 1);
            return true;
        }
    }

    // None of them fits. Truncate the one written for the narrowest case.
    const char* src = nullptr;
    for (int i = count - 1; i >= 0; --i) {
        if (candidates[i] != nullptr && candidates[i][0] != '\0') {
            src = candidates[i];
            break;
        }
    }
    if (src == nullptr) return false;

    const size_t ell = strlen(kWelcomeEllipsis);
    if (ell + 2 > cap) return false;   // no room for one character and a cut

    // Longest prefix whose rendering, ellipsis included, still fits. Linear
    // from the longest end rather than a binary search: the strings are short,
    // and a monotonic width is an assumption about the font this file does not
    // get to make.
    size_t best = 0;
    char scratch[128];
    const size_t room = (cap - 1 < sizeof(scratch) - 1 ? cap - 1 : sizeof(scratch) - 1);
    if (room <= ell) return false;
    size_t take = strlen(src);
    if (take > room - ell) take = room - ell;

    for (size_t n = take; n > 0; --n) {
        if (IsUtf8Continuation(src[n])) continue;   // mid-character: not a cut
        memcpy(scratch, src, n);
        memcpy(scratch + n, kWelcomeEllipsis, ell + 1);
        if (measure(scratch, ctx) <= max_width) {
            best = n;
            break;
        }
    }
    if (best == 0) return false;

    memcpy(out, src, best);
    memcpy(out + best, kWelcomeEllipsis, ell + 1);
    return true;
}

void WelcomeSwatchLayout(PaletteSwatch out[kWelcomeSwatchCount]) {
    if (out == nullptr) return;
    const Color colors[kWelcomeSwatchCount] = {WHITE, BLACK, YELLOW, RED};
    const int cell_w = kWelcomeSwatchRow.w / kWelcomeSwatchCount;
    for (int i = 0; i < kWelcomeSwatchCount; ++i) {
        const int cell_x = kWelcomeSwatchRow.x + i * cell_w;
        out[i].color = colors[i];
        // WHITE is the same colour as the canvas. Without a thicker border the
        // operator sees three swatches and a gap, and concludes the panel has
        // three colours.
        out[i].border_px = (colors[i] == WHITE) ? 2 : 1;
        out[i].chip = Rect{cell_x + (cell_w - kSwatchChip) / 2,
                           kWelcomeSwatchRow.y, kSwatchChip, kSwatchChip};
        out[i].cell_x = cell_x;
        out[i].cell_w = cell_w;
    }
}

void DrawWelcomeSwatches(uint8_t* fb, int width, int height) {
    if (fb == nullptr) return;
    SetFramebufferHeightHint(height);

    PaletteSwatch sw[kWelcomeSwatchCount];
    WelcomeSwatchLayout(sw);
    for (int i = 0; i < kWelcomeSwatchCount; ++i) {
        // Literal colours, straight through. Not ThemeManager, not
        // DrawDitherRect, not DrawStripeRect: this row's entire job is to show
        // what the four pigments actually look like on this glass, and anything
        // that maps or halftones them would make it a picture of the theme.
        DrawRect(fb, width, sw[i].chip, sw[i].color);
        DrawRectBorder(fb, width, sw[i].chip, sw[i].border_px, BLACK);
    }
}

}  // namespace rawdraw
