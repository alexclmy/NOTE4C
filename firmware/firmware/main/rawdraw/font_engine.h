/**
 * @file font_engine.h
 * @brief Minimal font engine extracted from LVGL (no LVGL runtime dependency)
 *
 * When LVGL is included first, this header uses LVGL's real types.
 * When LVGL is NOT included, it provides minimal standalone definitions.
 *
 * Reference: LVGL src/font/lv_font.h, src/font/lv_font_fmt_txt.c
 */

#ifndef RAWDRAW_FONT_ENGINE_H
#define RAWDRAW_FONT_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================
// Detect whether LVGL types are already available
// ============================================================
// LV_FONT_DECLARE is defined by LVGL's lv_font.h.
// If it exists, LVGL types are available. If not, provide our own.
#ifdef LV_FONT_DECLARE
    #define RAWDRAW_HAS_LVGL 1
#else
    #define RAWDRAW_HAS_LVGL 0
#endif

// ============================================================
// Type definitions — only when LVGL is NOT present
// ============================================================
#if !RAWDRAW_HAS_LVGL

/* Forward declarations for v9 types (we only pass NULL for draw_buf) */
struct _lv_draw_buf_t;
typedef void lv_cache_entry_t;

typedef enum {
    LV_FONT_GLYPH_FORMAT_A1 = 0x01,
    LV_FONT_GLYPH_FORMAT_A8 = 0x08,
} lv_font_glyph_format_t;

typedef struct {
    const struct _lv_font_t * resolved_font; /**< Pointer to font where glyph was found */
    uint16_t adv_w; /**< The glyph needs this space */
    uint16_t box_w; /**< Width of the glyph's bounding box */
    uint16_t box_h; /**< Height of the glyph's bounding box */
    int16_t  ofs_x; /**< x offset of the bounding box */
    int16_t  ofs_y; /**< y offset of the bounding box */
    uint16_t stride; /**< Bytes in each line */
    lv_font_glyph_format_t format; /**< Font format of the glyph */
    uint8_t is_placeholder : 1; /**< Glyph is missing but placeholder will be displayed */
    uint8_t req_raw_bitmap : 1; /**< 1: return raw bitmap, 0: return A8/ARGB8888 */
    int32_t outline_stroke_width; /**< Used with freetype vector fonts */
    union {
        uint32_t index;       /**< Unicode code point / glyph index */
        const void * src;     /**< Pointer to source data for image fonts */
    } gid;
    lv_cache_entry_t * entry; /**< Cache entry of the glyph draw data */
} lv_font_glyph_dsc_t;

typedef struct _lv_font_t {
    bool (*get_glyph_dsc)(const struct _lv_font_t * font, lv_font_glyph_dsc_t * dsc_out,
                          uint32_t letter, uint32_t letter_next);
    const void * (*get_glyph_bitmap)(lv_font_glyph_dsc_t * g_dsc, struct _lv_draw_buf_t * draw_buf);
    int32_t line_height;
    int32_t base_line;
    uint8_t subpx : 2;
    uint8_t kerning : 1;
    uint8_t static_bitmap : 1;
    int8_t underline_position;
    int8_t underline_thickness;
    const void * dsc;
    const struct _lv_font_t * fallback;
    void * user_data;
} lv_font_t;

#define LV_FONT_FMT_PLAIN       0
#define LV_FONT_FMT_COMPRESSED  1

static inline bool lv_font_get_glyph_dsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out,
                                         uint32_t letter, uint32_t letter_next) {
    if (!font || !font->get_glyph_dsc) return false;
    return font->get_glyph_dsc(font, dsc_out, letter, letter_next);
}

static inline const void* lv_font_get_glyph_bitmap(lv_font_glyph_dsc_t* g_dsc,
                                                   struct _lv_draw_buf_t* draw_buf) {
    if (!g_dsc || !g_dsc->resolved_font || !g_dsc->resolved_font->get_glyph_bitmap) return NULL;
    return g_dsc->resolved_font->get_glyph_bitmap(g_dsc, draw_buf);
}

#ifndef LV_FONT_DECLARE
#define LV_FONT_DECLARE(font_name) extern const lv_font_t font_name;
#endif

#endif /* !RAWDRAW_HAS_LVGL */

// ============================================================
// FONT_DECLARE — always available (alias for LV_FONT_DECLARE)
// ============================================================
#ifndef FONT_DECLARE
#define FONT_DECLARE LV_FONT_DECLARE
#endif

// ============================================================
// UTF-8 Decoder (always provided — not in LVGL's public API)
// ============================================================

static inline uint32_t utf8_next(const char** pp) {
    const uint8_t* p = (const uint8_t*)*pp;
    if (*p == 0) return 0;

    uint32_t c;
    int len;

    if (*p < 0x80) {
        c = *p;
        len = 1;
    } else if ((*p & 0xE0) == 0xC0) {
        c = *p & 0x1F;
        len = 2;
    } else if ((*p & 0xF0) == 0xE0) {
        c = *p & 0x0F;
        len = 3;
    } else if ((*p & 0xF8) == 0xF0) {
        c = *p & 0x07;
        len = 4;
    } else {
        *pp += 1;
        return 0xFFFD;
    }

    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp += 1;
            return 0xFFFD;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }

    *pp += len;
    return c;
}

// ============================================================
// Common Font Declarations
// ============================================================

FONT_DECLARE(SourceHanSansSC_Regular_slim);
FONT_DECLARE(SourceHanSansSC_Medium_slim);
FONT_DECLARE(font_zectrix_16_1);
FONT_DECLARE(font_zectrix_48_1);
FONT_DECLARE(weather_icons_16);
FONT_DECLARE(weather_icons_48);
// fa_settings_16 is declared in fa_settings.h with C linkage
FONT_DECLARE(BUILTIN_TEXT_FONT);

#endif /* RAWDRAW_FONT_ENGINE_H */
