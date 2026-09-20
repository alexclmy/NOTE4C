/*
 * GENERATED FILE - DO NOT EDIT.
 *
 * Produced by note4c-control-tower/tools/gen_firmware_font.py from the
 * committed OFL atlas src/core/render/fonts/inter.json. Re-run that script
 * to change it; editing this file by hand would break the byte-for-byte
 * parity between the device's compositor and the tower's preview, which is
 * the one thing the golden-frame test exists to catch.
 *
 * Glyph data derived from Inter, (c) 2020 The Inter Project Authors.
 * SIL Open Font License 1.1 - see THIRD_PARTY_NOTICES.md. The OFL is a
 * separate obligation from the MIT licence of the surrounding code.
 *
 * 1096 glyphs across 8 atlases, 29448 bytes of bitmap.
 * bitmap sha256: b9bef776c103a8f8af1eb13bd36dde8462a2e89b4be91ed3c6ff172997f2c73c
 */

#ifndef COMMON_AUTONOMY_FONT_DATA_H
#define COMMON_AUTONOMY_FONT_DATA_H

#include <stddef.h>
#include <stdint.h>

namespace autonomy {
namespace font {

/// One glyph. `left`/`top` are the bearings the atlas recorded; `advance`
/// is the whole-pixel pen movement, already grid-fitted by the rasteriser.
struct Glyph {
    uint16_t codepoint;
    uint8_t width;
    uint8_t height;
    int8_t left;
    int8_t top;
    uint8_t advance;
    uint32_t bitmap_offset;
    uint16_t bitmap_len;
};

/// One size and weight. Glyphs are sorted by code point so a lookup is a
/// binary search rather than a scan of several hundred entries per character.
struct Atlas {
    const Glyph* glyphs;
    uint16_t glyph_count;
    uint8_t ascent;
    uint8_t descent;
    uint8_t size;
    bool bold;
};

/// Row-major, 1 bit per pixel, MSB first, stride = (width + 7) / 8.
extern const uint8_t kGlyphBits[];
constexpr size_t kGlyphBitsLen = 29448;

constexpr size_t kSizeCount = 4;
constexpr uint8_t kSizes[kSizeCount] = {11, 15, 22, 34};

/// Look up an atlas by size and weight, or nullptr when the ladder has no
/// such rung. The compositor walks kSizes downwards, so it asks for sizes it
/// knows exist; the null return is for callers that compute one.
const Atlas* Find(uint8_t size, bool bold);

/// Look up one glyph in @p atlas, or nullptr when the subset has no such
/// character. Both renderers drop unknown characters identically, which is
/// why the subset is generated into both rather than chosen twice.
const Glyph* FindGlyph(const Atlas& atlas, uint32_t codepoint);

}  // namespace font
}  // namespace autonomy

#endif  // COMMON_AUTONOMY_FONT_DATA_H
