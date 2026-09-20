/**
 * @file octopus_mark.cc
 * @brief The mascot bitmap and the blit. See the header for its provenance.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "octopus_mark.h"

namespace rawdraw {

namespace {

/**
 * @brief The body, transcribed from OCTOPUS_BODY in sprites.ts.
 *
 * Thirteen columns by twelve rows: a domed head, then three rows that split
 * into the legs. Kept as strings rather than packed bits so a change to the
 * artwork is readable in a diff and can be compared by eye against the
 * TypeScript it mirrors. Pasted at kOctopusInk's origin, exactly as
 * `octopusSprite()` pastes it at (7, 9).
 */
const char* const kBodyRows[12] = {
    "0001111111000",
    "0011111111100",
    "0111111111110",
    "1111111111111",
    "1111111111111",
    "1111111111111",
    "1111111111111",
    "0111111111110",
    "0011111111100",
    "0110110110110",
    "1100110110011",
    "1001100011001",
};

/// Sprite-grid x of the two eyes, from `for (const xx of [10, 16])`.
constexpr int kEyeX[2] = {10, 16};
/// Each eye is the inclusive rectangle (x, 13)..(x + 2, 16) in BLACK, with the
/// top-left pixel set back to WHITE — the highlight that stops the eye reading
/// as a hole, and the only part of the face that survives a 1bpp panel.
constexpr int kEyeTop = 13;
constexpr int kEyeBottom = 16;
constexpr int kEyeWidth = 3;

/// The mouth: the inclusive horizontal run (12, 18)..(15, 18) in BLACK.
constexpr int kMouthY = 18;
constexpr int kMouthX0 = 12;
constexpr int kMouthX1 = 15;

/// One sprite grid, in palette indices. 754 bytes on the stack, built the same
/// way every call, because the composed frame is compared byte for byte before
/// a twenty-five-second refresh is spent on it.
struct Sprite {
    Color px[kOctopusSpriteW * kOctopusSpriteH];

    void Set(int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= kOctopusSpriteW || y >= kOctopusSpriteH) return;
        px[y * kOctopusSpriteW + x] = c;
    }
    Color At(int x, int y) const { return px[y * kOctopusSpriteW + x]; }
};

/**
 * @brief Build the plain mascot: body, eyes, mouth, and nothing else.
 *
 * The order matters and is the source's: the body is laid down first, then the
 * face is punched into it. Drawing the face first would leave it under the red.
 */
void BuildPlainSprite(Sprite& s) {
    for (int i = 0; i < kOctopusSpriteW * kOctopusSpriteH; ++i) {
        s.px[i] = WHITE;
    }

    for (int y = 0; y < kOctopusInk.h; ++y) {
        for (int x = 0; x < kOctopusInk.w; ++x) {
            if (kBodyRows[y][x] == '1') {
                s.Set(kOctopusInk.x + x, kOctopusInk.y + y, RED);
            }
        }
    }

    for (int eye = 0; eye < 2; ++eye) {
        const int x0 = kEyeX[eye];
        for (int y = kEyeTop; y <= kEyeBottom; ++y) {
            for (int x = x0; x < x0 + kEyeWidth; ++x) {
                s.Set(x, y, BLACK);
            }
        }
        s.Set(x0, kEyeTop, WHITE);
    }

    for (int x = kMouthX0; x <= kMouthX1; ++x) {
        s.Set(x, kMouthY, BLACK);
    }
}

}  // namespace

Rect OctopusInkBounds(const Point& ink_center, int scale) {
    const int w = kOctopusInk.w * scale;
    const int h = kOctopusInk.h * scale;
    return Rect{ink_center.x - w / 2, ink_center.y - h / 2, w, h};
}

Rect OctopusBounds(const Point& ink_center, int scale) {
    const Rect ink = OctopusInkBounds(ink_center, scale);
    return Rect{ink.x - kOctopusInk.x * scale, ink.y - kOctopusInk.y * scale,
                kOctopusSpriteW * scale, kOctopusSpriteH * scale};
}

void DrawOctopusMark(uint8_t* fb, int width, int height, const Point& ink_center,
                     int scale) {
    if (fb == nullptr || scale < kOctopusMinScale) return;

    // The pixel writers guard y against this hint rather than against a height
    // they are passed, so it has to be set before anything is drawn. The same
    // thing DrawRoundRect does, for the same reason.
    SetFramebufferHeightHint(height);

    const Rect bounds = OctopusBounds(ink_center, scale);
    if (bounds.x < 0 || bounds.y < 0 || bounds.x + bounds.w > width ||
        bounds.y + bounds.h > height) {
        return;
    }

    Sprite s;
    BuildPlainSprite(s);

    // Nearest-neighbour at an integer factor, which is a block fill: no
    // resampling, no rounding, and the pixel edges stay hard.
    for (int sy = 0; sy < kOctopusSpriteH; ++sy) {
        for (int sx = 0; sx < kOctopusSpriteW; ++sx) {
            const Color c = s.At(sx, sy);
            const int x0 = bounds.x + sx * scale;
            const int y0 = bounds.y + sy * scale;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    set_pixel(fb, width, x0 + dx, y0 + dy, c);
                }
            }
        }
    }
}

}  // namespace rawdraw
