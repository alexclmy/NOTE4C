/**
 * @file rawdraw.h
 * @brief Pure 2bpp framebuffer drawing API (no LVGL dependency)
 *
 * This module provides low-level drawing functions that operate directly
 * on a 2bpp (2 bits per pixel) framebuffer. All functions are pure and
 * take the framebuffer pointer as parameter.
 *
 * Key concepts:
 * - 2bpp format: 00=BLACK, 01=WHITE, 10=YELLOW, 11=RED
 * - x coordinates must be 8-byte aligned for EPD hardware (use align_x8)
 * - All drawing is immediate (no buffering/clipping by this layer)
 *
 * Reference: custom_lcd_display.cc lines 89-96 (pixel packing helpers), 1292-1347 (render_text_to_buffer)
 */

#ifndef RAWDRAW_RAWDRAW_H
#define RAWDRAW_RAWDRAW_H

#include <stdint.h>
#include <stdbool.h>
#include "font_engine.h"

namespace rawdraw {

// ============================================================
// Geometric Types
// ============================================================

/**
 * @brief Rectangle with position and size
 */
struct Rect {
    int x;      /**< Left edge x coordinate */
    int y;      /**< Top edge y coordinate */
    int w;      /**< Width (pixels) */
    int h;      /**< Height (pixels) */
};

/**
 * @brief Point coordinate
 */
struct Point {
    int x;      /**< X coordinate */
    int y;      /**< Y coordinate */
};

// ============================================================
// Color (2bpp quad color)
// ============================================================

/**
 * @brief Color values for 2bpp framebuffer
 */
enum Color {
    BLACK = 0,     /**< Black pixel (bit clear) */
    WHITE = 1,     /**< White pixel */
    YELLOW = 2,    /**< Yellow pixel */
    RED = 3,       /**< Red pixel */
};

// ============================================================
// Rectangle Utilities (from custom_lcd_display.cc)
// ============================================================

/**
 * @brief Calculate rectangle area
 * @return Area in pixels, 0 if invalid
 */
int rect_area(const Rect& r);

/**
 * @brief Union of two rectangles (smallest enclosing rectangle)
 */
Rect rect_union(const Rect& a, const Rect& b);

/**
 * @brief Clamp rectangle to framebuffer bounds
 * @param width Framebuffer width
 * @param height Framebuffer height
 */
Rect clamp_rect(const Rect& r, int width, int height);

/**
 * @brief Align rectangle x coordinates to 8-byte boundary (EPD hardware requirement)
 */
Rect align_x8(const Rect& r);

// ============================================================
// Pixel Operations
// ============================================================

/**
 * @brief Set framebuffer height hint for bounds-safe pixel operations
 *
 * Most rawdraw APIs only pass width to keep callsites lightweight.
 * This hint is used by set_pixel/get_pixel to guard y bounds and
 * prevent accidental framebuffer overrun.
 */
void SetFramebufferHeightHint(int height);

/**
 * @brief Set a single pixel in 2bpp framebuffer
 *
 * @param fb Framebuffer pointer (2bpp format)
 * @param width Framebuffer width in pixels
 * @param x X coordinate (0 to width-1)
 * @param y Y coordinate (0 to height-1)
 * @param color BLACK, WHITE, YELLOW, or RED
 *
 * Framebuffer layout: (width*2+7)/8 bytes per row, packed MSB-first
 * - Each byte represents 4 horizontal pixels
 * - Pixel 0 occupies bits 7-6, pixel 1 occupies bits 5-4, etc.
 */
void set_pixel(uint8_t* fb, int width, int x, int y, Color color);

/**
 * @brief Set a single pixel in 2bpp framebuffer
 */
void set_pixel_2bpp(uint8_t* fb, int width, int x, int y, Color color);

/**
 * @brief Get pixel value from 2bpp framebuffer
 */
Color get_pixel(const uint8_t* fb, int width, int x, int y);

/**
 * @brief Get pixel value from 2bpp framebuffer
 */
Color get_pixel_2bpp(const uint8_t* fb, int width, int x, int y);

// ============================================================
// Basic Shapes
// ============================================================

/**
 * @brief Draw filled rectangle
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param r Rectangle bounds
 * @param color Fill color (WHITE or BLACK)
 */
void DrawRect(uint8_t* fb, int width, const Rect& r, Color color);

/**
 * @brief Draw dithered rectangle (checkerboard pattern for 50% gray effect)
 *
 * Creates a visual "gray" effect on 2bpp displays by alternating black/white pixels.
 * Pattern: (x + y) % 2 == 0 → black, otherwise white.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param r Rectangle bounds
 */
void DrawDitherRect(uint8_t* fb, int width, const Rect& r);

/**
 * @brief Draw horizontal stripe selection fill (1px black / 1px white)
 *
 * Matches the HTML design spec's repeating-linear-gradient(0deg, ...).
 */
void DrawStripeRect(uint8_t* fb, int width, const Rect& r);

/**
 * @brief Draw rectangle outline (border only)
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param r Rectangle bounds
 * @param thickness Border thickness in pixels (default 1)
 * @param color Border color
 */
void DrawRectBorder(uint8_t* fb, int width, const Rect& r, int thickness, Color color);

/**
 * @brief Draw rounded rectangle (filled) - full version with height
 *
 * Uses midpoint circle algorithm for corner arcs.
 * All coordinates are clamped to framebuffer bounds to prevent overflow.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param height Framebuffer height (for boundary clamping)
 * @param r Rectangle bounds
 * @param radius Corner radius (clamped to min(w,h)/2)
 * @param fill_color Interior fill color
 * @param border_color Border color (if different from fill)
 * @param border_thickness Border thickness (0 for no border)
 */
void DrawRoundRect(uint8_t* fb, int width, int height, const Rect& r, int radius,
                   Color fill_color, Color border_color = BLACK, int border_thickness = 1);

/**
 * @brief Draw rounded rectangle (filled) - backward compatible version
 *
 * Calls the full version with height=300 (default for 400x300 EPD).
 */
void DrawRoundRect(uint8_t* fb, int width, const Rect& r, int radius,
                   Color fill_color, Color border_color = BLACK, int border_thickness = 1);

/**
 * @brief Draw rounded rectangle outline (border only, no fill) - full version
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param height Framebuffer height (for boundary clamping)
 * @param r Rectangle bounds
 * @param radius Corner radius
 * @param thickness Border thickness
 * @param color Border color
 */
void DrawRoundRectBorder(uint8_t* fb, int width, int height, const Rect& r, int radius, int thickness, Color color);

/**
 * @brief Draw rounded rectangle outline (border only) - backward compatible version
 *
 * Calls the full version with height=300.
 */
void DrawRoundRectBorder(uint8_t* fb, int width, const Rect& r, int radius, int thickness, Color color);

/**
 * @brief Draw horizontal line
 *
 * Optimized for filling consecutive pixels in a row.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param y Y coordinate
 * @param x1 Start x (inclusive)
 * @param x2 End x (inclusive)
 * @param color Line color
 */
void DrawHLine(uint8_t* fb, int width, int y, int x1, int x2, Color color);

/**
 * @brief Draw vertical line
 */
void DrawVLine(uint8_t* fb, int width, int x, int y1, int y2, Color color);

/**
 * @brief Draw arbitrary line (Bresenham's algorithm)
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param p1 Start point
 * @param p2 End point
 * @param color Line color
 */
void DrawLine(uint8_t* fb, int width, const Point& p1, const Point& p2, Color color);

/**
 * @brief Draw filled circle
 *
 * Uses midpoint circle algorithm.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param center Circle center point
 * @param radius Radius in pixels
 * @param color Fill color
 */
void DrawCircle(uint8_t* fb, int width, const Point& center, int radius, Color color);

/**
 * @brief Draw circle outline
 */
void DrawCircleBorder(uint8_t* fb, int width, const Point& center, int radius, int thickness, Color color);

// ============================================================
// Text Rendering (reuses LVGL font infrastructure)
// ============================================================

/**
 * @brief Draw text using LVGL-generated font
 *
 * This function directly writes glyph bitmaps to the framebuffer.
 * It reuses the logic from custom_lcd_display.cc:render_text_to_buffer().
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param x Start x position (left edge of text)
 * @param y Start y position (top edge, not baseline)
 * @param text UTF-8 encoded text string
 * @param font LVGL font descriptor (must have get_glyph_dsc and get_glyph_bitmap)
 * @param color Text color (BLACK for normal text on white background)
 *
 * Supported features:
 * - UTF-8 decoding (CJK, ASCII, icons)
 * - Newline handling ('\n' advances to next line)
 * - Glyph positioning (ofs_x, ofs_y, adv_w)
 * - Both plain and stride-aligned bitmap formats
 */
void DrawText(uint8_t* fb, int width, int x, int y, const char* text,
              const lv_font_t* font, Color color = BLACK,
              int height = 300);  ///< FB height for y-clipping

/**
 * @brief Draw single icon from icon font
 *
 * Icon fonts (font_zectrix, weather_icons) contain single glyph per character.
 * Use this for drawing icons at specific positions with specific size.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param x Icon position x
 * @param y Icon position y (top edge)
 * @param icon_code UTF-8 icon character code (e.g., "\xee\xa4\x80" for WiFi icon)
 * @param font Icon font (font_zectrix_16_1, weather_icons_48, etc.)
 * @param color Icon color
 */
void DrawIcon(uint8_t* fb, int width, int x, int y, const char* icon_code,
              const lv_font_t* font, Color color = BLACK);

// ============================================================
// Progress/Status Indicators
// ============================================================

/**
 * @brief Draw progress bar (horizontal, rounded corners)
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param r Bar bounds (full bar area)
 * @param value_pct Progress value (0-100)
 * @param bg_color Background (empty area) color
 * @param fg_color Foreground (filled area) color
 * @param radius Corner radius (default: bar height/2 for pill shape)
 */
void DrawProgress(uint8_t* fb, int width, const Rect& r, int value_pct,
                  Color bg_color = WHITE, Color fg_color = BLACK, int radius = -1);

/**
 * @brief Draw horizontal progress bar with text label
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param x Position x
 * @param y Position y
 * @param w Bar width
 * @param h Bar height
 * @param value_pct Progress value (0-100)
 * @param label Optional text label (centered on bar)
 * @param font Label font
 */
void DrawProgressWithLabel(uint8_t* fb, int width, int x, int y, int w, int h,
                           int value_pct, const char* label, const lv_font_t* font);

// ============================================================
// Measurement Utilities
// ============================================================

/**
 * @brief Measure text width in pixels
 *
 * @param text UTF-8 text to measure
 * @param font Font to use for measurement
 * @return Width in pixels (does not include trailing whitespace)
 */
int MeasureTextWidth(const char* text, const lv_font_t* font);

/**
 * @brief Measure text height (line height from font)
 */
int MeasureTextHeight(const lv_font_t* font);

/**
 * @brief Calculate text bounds (width and height for multiline text)
 *
 * @param text UTF-8 text (may contain '\n')
 * @param font Font to use
 * @param max_width Maximum width for wrapping (0 = no wrap)
 * @return Rect with w = max line width, h = total height
 */
Rect MeasureTextBounds(const char* text, const lv_font_t* font, int max_width = 0);

// ============================================================
// Region Operations
// ============================================================

/**
 * @brief Fill region with color
 *
 * Equivalent to DrawRect, but named for clarity when filling existing area.
 */
void FillRect(uint8_t* fb, int width, const Rect& r, Color color);

/**
 * @brief Invert region (XOR each pixel)
 *
 * Useful for highlighting selections or creating contrast.
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param r Region to invert
 */
void InvertRegion(uint8_t* fb, int width, const Rect& r);

/**
 * @brief Copy region from one framebuffer to another
 *
 * @param src Source framebuffer
 * @param dst Destination framebuffer
 * @param width Framebuffer width (must be same for src and dst)
 * @param src_r Source region
 * @param dst_x Destination x position
 * @param dst_y Destination y position
 */
void CopyRegion(const uint8_t* src, uint8_t* dst, int width,
                const Rect& src_r, int dst_x, int dst_y);

/**
 * @brief Clear framebuffer to white
 *
 * @param fb Framebuffer pointer
 * @param width Framebuffer width
 * @param height Framebuffer height
 */
void Clear(uint8_t* fb, int width, int height, Color fill = WHITE);

}  // namespace rawdraw

#endif  // RAWDRAW_RAWDRAW_H
