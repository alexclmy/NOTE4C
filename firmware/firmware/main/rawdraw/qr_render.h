/**
 * @file qr_render.h
 * @brief Draw a QR code into a 2bpp framebuffer, or draw nothing and say so.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * Encoding is Nayuki's qrcodegen (MIT), vendored verbatim in this directory.
 * See THIRD_PARTY_NOTICES.md.
 *
 * WHAT THIS IS FOR, AND WHAT IT MUST NEVER CARRY
 * ----------------------------------------------
 * One caller: the welcome screen, encoding `http://<lan ip>/` — the address a
 * human types into the Control Tower to pair with this device. That is the only
 * string this device has that is both knowable to it and safe to display.
 *
 * It must never encode the dashboard pairing token or any other credential.
 * E-paper holds its last image with the power off, so anything drawn here is
 * effectively written on a card and left on the desk; that is the same rule
 * dashboard_service.h states for the pairing window, and it is the reason this
 * function takes a plain string from a caller that has no way to reach the
 * token rather than a "render the pairing QR" flag.
 *
 * WHY THE MODULES ARE ONLY EVER BLACK AND WHITE
 * ---------------------------------------------
 * Not a style choice. This board has a 1bpp panel variant
 * (`ZECTRIX_EPD_PANEL_1BPP`), and its packing path collapses BLACK, RED and
 * YELLOW all to black — only WHITE stays white. A red or yellow module would
 * look fine in the 4-colour preview and would be a solid black square, and
 * therefore unscannable, on the other panel. So the colours are not parameters,
 * and a host test reads the drawn pixels back to prove it.
 *
 * Free of ESP-IDF, LVGL and the font engine: it plots rectangles and nothing
 * else, which is what lets tests/host/run.sh compile it.
 */

#ifndef RAWDRAW_QR_RENDER_H
#define RAWDRAW_QR_RENDER_H

#include "rawdraw.h"

namespace rawdraw {

/// Quiet zone, in modules, on every side. Four is what the QR specification
/// requires; a code drawn hard against other ink is a code a phone gives up on.
constexpr int kQrQuietModules = 4;

/// The smallest module we will draw. Below this the panel's own dot pitch and
/// the camera's angle stop resolving them, and a QR that cannot be scanned is
/// worse than a printed URL because it looks like it should work.
constexpr int kQrMinModulePx = 3;

/**
 * @brief Encode @p text and draw it centred in @p box.
 *
 * @param fb,width,height the 2bpp framebuffer and its dimensions.
 * @param box    the reserved area. The code is drawn as large as fits, quiet
 *               zone included *inside* this box, and centred in it.
 * @param text   what to encode. Must be NUL-terminated and non-empty.
 * @param size_out optional: the rendered square's side in pixels.
 *
 * @return false, having drawn nothing at all, when the text is empty, will not
 *         fit the version ceiling, or cannot be drawn at @c kQrMinModulePx
 *         inside @p box. Drawing nothing is deliberate: a clipped or
 *         two-pixel-per-module QR is indistinguishable from a working one until
 *         somebody stands in front of the device trying to scan it, and the
 *         caller has an honest fallback to draw instead.
 *
 * Deterministic. The same text and box always produce the same pixels, which is
 * what lets the dedup in front of a 25-second panel refresh do its job.
 */
bool DrawQrCode(uint8_t* fb, int width, int height, const Rect& box,
                const char* text, int* size_out = nullptr);

}  // namespace rawdraw

#endif  // RAWDRAW_QR_RENDER_H
