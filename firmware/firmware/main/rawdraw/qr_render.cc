/**
 * @file qr_render.cc
 * @brief Implementation of DrawQrCode. See qr_render.h for the rules.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "qr_render.h"

#include <string.h>

extern "C" {
#include "qrcodegen.h"
}

namespace rawdraw {

namespace {

/**
 * @brief Version ceiling, and why it is 6 rather than 40.
 *
 * Version 6 is 41x41 modules. With the four-module quiet zone on each side that
 * is 49 across, and in the welcome screen's 168-pixel box that leaves 3 pixels
 * per module — exactly the floor below which a phone camera stops resolving
 * them at arm's length on a coarse e-paper panel.
 *
 * So the ceiling is not a memory budget, it is the point past which a QR this
 * screen drew would be decoration. A longer address than that — an IPv6 literal
 * is the realistic case — earns `false` and the caller's honest fallback text
 * rather than a square nobody can scan.
 *
 * Version 6 at ECC medium holds 134 bytes, which is far more than
 * "http://<panel-ip>/" needs; the binding constraint is the pixels, not the
 * capacity.
 */
constexpr int kMaxVersion = 6;

/// Scratch for the encoder. Sized by its own macro so the two cannot drift.
constexpr size_t kQrBufferLen = qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion);

}  // namespace

bool DrawQrCode(uint8_t* fb, int width, int height, const Rect& box,
                const char* text, int* size_out) {
    if (size_out != nullptr) *size_out = 0;
    if (fb == nullptr || text == nullptr || text[0] == '\0') return false;
    if (box.w <= 0 || box.h <= 0 || width <= 0 || height <= 0) return false;

    // The pixel writers guard `y` against this hint rather than against a
    // height they are passed. Set before anything is drawn, as DrawRoundRect
    // does.
    SetFramebufferHeightHint(height);

    // Encoded into locals: this runs on the render task, once, and 1.2 KB of
    // stack costs nothing next to the framebuffer it is about to draw into.
    uint8_t qr[kQrBufferLen];
    uint8_t tmp[kQrBufferLen];

    // ECC medium rather than low. The source render has no smudge, but the
    // panel is coarse and thresholded, and a photograph of it at an oblique
    // angle loses modules; 15% of redundancy costs one version at this payload
    // length and buys the difference between "usually scans" and "scans".
    //
    // Zero user input reaches this: the caller builds the string from the
    // device's own IP.
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, kMaxVersion,
                              qrcodegen_Mask_AUTO, /*boostEcl=*/true)) {
        return false;
    }

    const int modules = qrcodegen_getSize(qr);
    if (modules <= 0) return false;
    const int total = modules + 2 * kQrQuietModules;

    // Integer division, floored: a fractional module would put the code's right
    // edge somewhere other than where its left edge implies, and the quiet zone
    // is only a quiet zone if it is the full four modules wide.
    const int fit = (box.w < box.h ? box.w : box.h);
    const int scale = fit / total;
    if (scale < kQrMinModulePx) return false;

    const int side = total * scale;
    const int origin_x = box.x + (box.w - side) / 2;
    const int origin_y = box.y + (box.h - side) / 2;
    if (origin_x < 0 || origin_y < 0 || origin_x + side > width ||
        origin_y + side > height) {
        return false;
    }

    // The quiet zone is painted, not assumed. The caller's background is
    // usually white already, but "usually" is how a QR ends up sitting on top
    // of a separator line that was drawn first.
    DrawRect(fb, width, Rect{origin_x, origin_y, side, side}, WHITE);

    for (int my = 0; my < modules; ++my) {
        for (int mx = 0; mx < modules; ++mx) {
            if (!qrcodegen_getModule(qr, mx, my)) continue;
            const Rect cell{origin_x + (mx + kQrQuietModules) * scale,
                            origin_y + (my + kQrQuietModules) * scale,
                            scale, scale};
            // BLACK, always. See the header: RED and YELLOW survive the 4-colour
            // panel and collapse to black on the 1bpp one, which would destroy
            // the contrast this depends on with nothing to show for it.
            DrawRect(fb, width, cell, BLACK);
        }
    }

    if (size_out != nullptr) *size_out = side;
    return true;
}

}  // namespace rawdraw
