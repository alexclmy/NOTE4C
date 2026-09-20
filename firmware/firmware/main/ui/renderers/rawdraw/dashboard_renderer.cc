/**
 * @file dashboard_renderer.cc
 * @brief Full-screen renderer for the composed poulailler dashboard frame.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "dashboard_renderer.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

#include "common/dashboard_slot.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/octopus_mark.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/welcome_screen.h"

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

const char* kTag = "DashboardRdr";

/// Bytes one 2bpp row occupies, matching set_pixel_2bpp's own arithmetic.
inline int BytesPerRow(int width) { return (width * 2 + 7) >> 3; }

/// The font, handed to FitWelcomeLine() as an opaque context so that
/// welcome_screen.cc — which a host suite compiles — never sees an lv_font_t.
int MeasureInFont(const char* text, void* ctx) {
    return MeasureTextWidth(text, static_cast<const lv_font_t*>(ctx));
}

/**
 * @brief Draw one line of the welcome screen at @p x, @p y, or draw nothing.
 *
 * @p candidates are ordered longest first and @p max_w is the room to the right
 * of @p x. Nothing this function draws extends past x + max_w, which is what
 * makes "no text leaves the panel" a property of the screen rather than of the
 * particular strings somebody last measured by eye.
 */
void DrawFittedLine(uint8_t* fb, int width, int height, int x, int y, int max_w,
                    const char* const* candidates, int count,
                    const lv_font_t* font) {
    char line[96];
    if (!FitWelcomeLine(candidates, count, max_w, &MeasureInFont,
                        const_cast<lv_font_t*>(font), line, sizeof(line))) {
        return;
    }
    DrawText(fb, width, x, y, line, font, BLACK, height);
}

}  // namespace

DashboardRenderer::~DashboardRenderer() {
    heap_caps_free(frame_);
    frame_ = nullptr;
}

void DashboardRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;

    if (frame_ == nullptr) {
        frame_ = static_cast<uint8_t*>(
            heap_caps_malloc(dashboard::kFrameBytes, MALLOC_CAP_SPIRAM));
        if (frame_ == nullptr) {
            ESP_LOGE(kTag, "PSRAM allocation for the dashboard frame failed");
        }
    }
    // A page switch back to the dashboard always warrants a full refresh: the
    // previous page's content must not ghost through the image.
    MarkFullRefresh();
}

bool DashboardRenderer::SetFrame(const uint8_t* frame) {
    if (frame == nullptr || frame_ == nullptr) {
        return false;
    }
    std::memcpy(frame_, frame, dashboard::kFrameBytes);
    has_frame_ = true;
    MarkFullRefresh();
    return true;
}

void DashboardRenderer::SetPlaceholderInfo(const std::string& ip, bool provisioned,
                                           bool lan_service_running) {
    ip_ = ip;
    provisioned_ = provisioned;
    lan_service_running_ = lan_service_running;
}

void DashboardRenderer::Render(uint8_t* fb, int width, int height) {
    if (fb == nullptr) return;

    const size_t fb_bytes = static_cast<size_t>(BytesPerRow(width)) * static_cast<size_t>(height);

    if (!has_frame_ || frame_ == nullptr) {
        RenderPlaceholder(fb, width, height);
        return;
    }

    // The stored frame is exactly this panel's layout, so the fast path is a
    // straight copy. If the geometry ever stops matching, say so on screen
    // rather than copying a wrong number of bytes into the framebuffer.
    if (fb_bytes != dashboard::kFrameBytes) {
        ESP_LOGE(kTag, "framebuffer is %ux%u (%u bytes), frame is %u bytes",
                 static_cast<unsigned>(width), static_cast<unsigned>(height),
                 static_cast<unsigned>(fb_bytes),
                 static_cast<unsigned>(dashboard::kFrameBytes));
        RenderPlaceholder(fb, width, height);
        return;
    }

    std::memcpy(fb, frame_, dashboard::kFrameBytes);
}

/**
 * @brief The welcome screen: what this device shows before it holds a panel.
 *
 * WHAT THE QR ENCODES, AND WHAT IT MUST NEVER ENCODE
 * --------------------------------------------------
 * `http://<this device's LAN IP>/`, and nothing else. That is the address a
 * person types into the Control Tower to pair with this device, and it is the
 * only address that is both knowable here and safe to display: the Tower binds
 * to loopback by default and never tells the device where it is, so there is no
 * "Tower URL" for this screen to point at.
 *
 * The dashboard pairing token is never drawn. E-paper holds its last image with
 * the power off, so anything put on this glass is effectively written on a card
 * and left on the desk — the same rule dashboard_service.h states for the
 * pairing window. This function has no way to reach a token and is not given
 * one; see tests/host/test_qr_render.cc.
 *
 * EVERY STRING HERE IS ENGLISH, AND EVERY ONE IS MEASURED
 * -------------------------------------------------------
 * The first hardware lot drew this column in French at fixed x positions and
 * every line of it ran off the right-hand edge of the glass — "Ecran e-paper 4
 * couleurs" lost its last two words to the bezel. Both halves of that are fixed
 * here and neither is fixed by choosing shorter words and hoping:
 *
 *   - the copy is English, which is what the rest of the device's vocabulary
 *     already is (`nav::StatusLine()`, the logs, the API). The slim Source Han
 *     Sans subset's Latin cmap is U+0020..U+007E plus a few dozen symbols, so
 *     unaccented English is also the only thing it can render faithfully;
 *   - nothing is drawn at a bare x. Every line goes through FitWelcomeLine()
 *     against the width of the box it belongs to, with shorter wordings behind
 *     it and an ellipsis behind those. A line that cannot fit is not drawn.
 *
 * The device's address is the one string nobody can write short. It is offered
 * as the full URL first and as the bare dotted quad second, because the QR
 * beside it already carries the complete link — the text is there for somebody
 * typing it, not for somebody scanning it.
 *
 * WHY THERE ARE THREE LINES AND NO BANNER
 * ---------------------------------------
 * The screen used to open with `NOTE4C` in a band of its own above a rule, and
 * on lot 3 the band ate it: the photograph shows six pixels of the title
 * against the top edge of the glass. So the name is now the first line of the
 * right column, at the one y on this screen that has been photographed
 * rendering a whole line of text, and the band and its rule are gone rather
 * than left behind empty. `4-color e-paper` went with them — the swatch row at
 * the foot of the screen shows the four pigments, and a line of text counting
 * them for the reader was the only copy here that said nothing the picture did
 * not. What is left is a name, an address and a state. See welcome_screen.h for
 * what the photograph actually measures, which is not what it looks like.
 *
 * WHY THE SWATCHES USE LITERAL COLOURS, AND CARRY NO WORDS
 * -------------------------------------------------------
 * The row exists to show the four pigments this panel actually has. Running it
 * through ThemeManager, or dithering it, would make it a picture of the theme
 * rather than of the hardware. WHITE gets a two-pixel black border because it
 * is otherwise invisible against the canvas. The labels that used to sit under
 * the chips are gone: they told a reader that a yellow square was yellow, and
 * on the panel they printed across the chips themselves.
 */
void DashboardRenderer::RenderPlaceholder(uint8_t* fb, int width, int height) {
    Clear(fb, width, height);

    const lv_font_t* title_font = &SourceHanSansSC_Medium_slim;
    const lv_font_t* body_font = &SourceHanSansSC_Regular_slim;

    // Everything stays within x:[20, 380], which clears the panel's physical
    // corner dead-zones (Style::kCornerSafeInset) at every y, because x never
    // enters the excluded band. This screen draws no status bar, so it does not
    // start at Style::kContentTop. The layout constants assume this panel's
    // 400 px; a narrower framebuffer tightens the right edge rather than
    // drawing past it.
    constexpr int kLeft = kWelcomeSafeLeft;
    const int right = (width - kWelcomeSafeLeft < kWelcomeSafeRight)
                          ? width - kWelcomeSafeLeft
                          : kWelcomeSafeRight;

    // ---- left column: the QR, or an honest reason there is not one ----------

    // The payload is built inside DrawWelcomeQr from the address alone. This
    // function never sees a URL, a template or a token, which is what makes
    // "the QR cannot carry a credential" structural rather than a convention.
    const bool drew_qr = DrawWelcomeQr(fb, width, height, ip_.c_str());

    if (drew_qr) {
        const char* const caption[] = {"Scan to connect", "Scan me"};
        DrawFittedLine(fb, width, height, kWelcomeCaptionBox.x,
                       kWelcomeCaptionBox.y, right - kWelcomeCaptionBox.x,
                       caption, 2, body_font);
    } else {
        // The box keeps its outline and its size so the layout does not move
        // between states. Two reasons land here and they are different facts,
        // so they get different words: no address at all, or an address the
        // encoder could not draw at a scannable size.
        DrawWelcomeQrFallbackBox(fb, width, height);

        const char* const no_wifi[kWelcomeQrTextLines][2] = {
            {"No Wi-Fi yet", "No Wi-Fi"},
            {"Hold UP + DOWN", "Hold UP+DOWN"},
            {"to set up", "to set up"},
        };
        const char* const too_long[kWelcomeQrTextLines][2] = {
            {"Address too long", "Address long"},
            {"for a QR code", "for a QR"},
            {"at this size", "at this size"},
        };
        const int text_w = kWelcomeQrBox.w - 2 * kWelcomeQrTextInset;
        for (int i = 0; i < kWelcomeQrTextLines; ++i) {
            DrawFittedLine(fb, width, height,
                           kWelcomeQrBox.x + kWelcomeQrTextInset,
                           kWelcomeQrBox.y + kWelcomeQrTextTop +
                               i * kWelcomeQrTextLineH,
                           text_w, ip_.empty() ? no_wifi[i] : too_long[i], 2,
                           body_font);
        }
    }

    // ---- right column: what this is, where it is, and how it is doing -------

    // The product name leads the column. It is not drawn in the band above the
    // QR any more and there is no rule under it: on lot 3 that band swallowed
    // the title whole, and the top of this screen is the one place on it whose
    // behaviour nobody has a photograph of. See welcome_screen.h.
    const char* const title[] = {"NOTE4C"};
    DrawFittedLine(fb, width, height, kWelcomeTitleBox.x, kWelcomeTitleBox.y,
                   right - kWelcomeTitleBox.x, title, 1, title_font);

    const int detail_x = kWelcomeDetailBox.x;
    const int detail_w = right - detail_x;
    int line_index = 0;
    const auto detail_y = [&]() {
        return kWelcomeDetailBox.y + line_index * kWelcomeDetailLineH;
    };

    if (!ip_.empty()) {
        // Longest first: the link as a person would type it, then the bare
        // address. The QR carries the complete URL either way.
        char full[96];
        snprintf(full, sizeof(full), "http://%s/", ip_.c_str());
        const char* const address[] = {full, ip_.c_str()};
        DrawFittedLine(fb, width, height, detail_x, detail_y(), detail_w,
                       address, 2, body_font);
    }
    ++line_index;

    // The three-way state, from the two booleans this renderer already owns.
    const char* const waiting[] = {"Waiting for Wi-Fi", "No Wi-Fi"};
    const char* const lan_off[] = {"Network service off", "LAN service off",
                                   "LAN off"};
    const char* const ready[] = {"Ready for a dashboard", "Ready to receive",
                                 "Ready"};
    const char* const* state = waiting;
    int state_count = 2;
    if (provisioned_ && !lan_service_running_) {
        state = lan_off;
        state_count = 3;
    } else if (provisioned_ && lan_service_running_) {
        state = ready;
        state_count = 3;
    }
    DrawFittedLine(fb, width, height, detail_x, detail_y(), detail_w, state,
                   state_count, body_font);

    DrawOctopusMark(fb, width, height, kWelcomeOctopusCenter,
                    kWelcomeOctopusScale);

    // ---- the four pigments, shown rather than described ---------------------

    DrawHLine(fb, width, kWelcomeSwatchRuleY, kLeft, right, BLACK);

    // Drawn entirely by the portable helper: the colours, the order and the
    // borders are what a host test reads back, and there is nothing left for
    // this file to add on top of them.
    DrawWelcomeSwatches(fb, width, height);
}

bool DashboardRenderer::HandleInput(const ButtonEvent& event) {
    // Navigation and the redraw gesture are owned by the UI manager and the
    // application, which know about the other pages. Consuming events here
    // would silently break page switching.
    (void)event;
    return false;
}

}  // namespace rawdraw
