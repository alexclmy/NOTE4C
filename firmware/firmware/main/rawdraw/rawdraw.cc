/**
 * @file rawdraw.cc
 * @brief Implementation of 2bpp framebuffer drawing functions
 *
 * All functions operate directly on the framebuffer pointer.
 * No LVGL dependency - uses extracted font_engine.h for text rendering.
 */

#include "rawdraw.h"
#include <cstring>
#include <algorithm>
#include <atomic>

namespace rawdraw {

namespace {
std::atomic<int> g_framebuffer_height_hint{300};
}

// ============================================================
// Rectangle Utilities (from custom_lcd_display.cc)
// ============================================================

int rect_area(const Rect& r) {
    return (r.w > 0 && r.h > 0) ? (r.w * r.h) : 0;
}

Rect rect_union(const Rect& a, const Rect& b) {
    if (rect_area(a) == 0) return b;
    if (rect_area(b) == 0) return a;
    int x1 = std::min(a.x, b.x);
    int y1 = std::min(a.y, b.y);
    int x2 = std::max(a.x + a.w, b.x + b.w);
    int y2 = std::max(a.y + a.h, b.y + b.h);
    return { x1, y1, x2 - x1, y2 - y1 };
}

Rect clamp_rect(const Rect& r, int width, int height) {
    int x1 = std::max(0, r.x);
    int y1 = std::max(0, r.y);
    int x2 = std::min(width, r.x + r.w);
    int y2 = std::min(height, r.y + r.h);
    return { x1, y1, x2 - x1, y2 - y1 };
}

Rect align_x8(const Rect& r) {
    Rect out = r;
    int x0 = (out.x / 8) * 8;
    int x1 = ((out.x + out.w + 7) / 8) * 8;
    out.x = x0;
    out.w = x1 - x0;
    return out;
}

// ============================================================
// Pixel Operations (2bpp framebuffer)
// ============================================================

void SetFramebufferHeightHint(int height) {
    if (height > 0) {
        g_framebuffer_height_hint.store(height, std::memory_order_relaxed);
    }
}

static inline uint8_t ColorToFillByte(Color color) {
    switch (color) {
        case BLACK: return 0x00;
        case WHITE: return 0x55;
        case YELLOW: return 0xAA;
        case RED: return 0xFF;
        default: return 0x55;
    }
}

void set_pixel_2bpp(uint8_t* fb, int width, int x, int y, Color color) {
    const int fb_height = g_framebuffer_height_hint.load(std::memory_order_relaxed);
    if (!fb || x < 0 || y < 0 || x >= width || y >= fb_height) return;

    uint16_t bytes_per_row = (width * 2 + 7) >> 3;
    uint32_t index = (uint32_t)y * bytes_per_row + (uint32_t)(x >> 2);
    uint8_t shift = (uint8_t)(6 - ((x & 0x03) << 1));
    uint8_t mask = (uint8_t)(0x03U << shift);
    fb[index] = (uint8_t)((fb[index] & (uint8_t)~mask) | ((uint8_t)color << shift));
}

void set_pixel(uint8_t* fb, int width, int x, int y, Color color) {
    set_pixel_2bpp(fb, width, x, y, color);
}

Color get_pixel_2bpp(const uint8_t* fb, int width, int x, int y) {
    const int fb_height = g_framebuffer_height_hint.load(std::memory_order_relaxed);
    if (!fb || x < 0 || y < 0 || x >= width || y >= fb_height) return WHITE;

    uint16_t bytes_per_row = (width * 2 + 7) >> 3;
    uint32_t index = (uint32_t)y * bytes_per_row + (uint32_t)(x >> 2);
    uint8_t shift = (uint8_t)(6 - ((x & 0x03) << 1));
    return static_cast<Color>((fb[index] >> shift) & 0x03);
}

Color get_pixel(const uint8_t* fb, int width, int x, int y) {
    return get_pixel_2bpp(fb, width, x, y);
}

// ============================================================
// Basic Shapes
// ============================================================

void DrawRect(uint8_t* fb, int width, const Rect& r, Color color) {
    if (!fb || r.w <= 0 || r.h <= 0) return;

    // Optimize horizontal fills: set multiple pixels at once
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            set_pixel(fb, width, x, y, color);
        }
    }
}

void DrawDitherRect(uint8_t* fb, int width, const Rect& r) {
    // Checkerboard pattern: (x + y) % 2 == 0 → black, otherwise white
    // Creates 50% gray visual effect on 2bpp displays
    if (!fb || r.w <= 0 || r.h <= 0) return;

    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            Color color = ((x + y) & 1) ? WHITE : BLACK;
            set_pixel(fb, width, x, y, color);
        }
    }
}

void DrawStripeRect(uint8_t* fb, int width, const Rect& r) {
    if (!fb || r.w <= 0 || r.h <= 0) return;
    for (int y = r.y; y < r.y + r.h; ++y) {
        const Color line_color = ((y - r.y) & 1) ? WHITE : BLACK;
        DrawHLine(fb, width, y, r.x, r.x + r.w - 1, line_color);
    }
}

void DrawRectBorder(uint8_t* fb, int width, const Rect& r, int thickness, Color color) {
    if (!fb || r.w <= 0 || r.h <= 0 || thickness <= 0) return;

    // Top and bottom edges
    for (int t = 0; t < thickness; t++) {
        DrawHLine(fb, width, r.y + t, r.x, r.x + r.w - 1, color);
        DrawHLine(fb, width, r.y + r.h - 1 - t, r.x, r.x + r.w - 1, color);
    }

    // Left and right edges
    for (int t = 0; t < thickness; t++) {
        DrawVLine(fb, width, r.x + t, r.y + thickness, r.y + r.h - thickness - 1, color);
        DrawVLine(fb, width, r.x + r.w - 1 - t, r.y + thickness, r.y + r.h - thickness - 1, color);
    }
}

void DrawHLine(uint8_t* fb, int width, int y, int x1, int x2, Color color) {
    if (!fb) return;
    if (x1 > x2) std::swap(x1, x2);
    for (int x = x1; x <= x2; x++) {
        set_pixel(fb, width, x, y, color);
    }
}

void DrawVLine(uint8_t* fb, int width, int x, int y1, int y2, Color color) {
    if (!fb) return;
    if (y1 > y2) std::swap(y1, y2);
    for (int y = y1; y <= y2; y++) {
        set_pixel(fb, width, x, y, color);
    }
}

void DrawLine(uint8_t* fb, int width, const Point& p1, const Point& p2, Color color) {
    if (!fb) return;

    // Bresenham's line algorithm
    int dx = std::abs(p2.x - p1.x);
    int dy = std::abs(p2.y - p1.y);
    int sx = (p1.x < p2.x) ? 1 : -1;
    int sy = (p1.y < p2.y) ? 1 : -1;
    int err = dx - dy;

    int x = p1.x, y = p1.y;

    while (true) {
        set_pixel(fb, width, x, y, color);

        if (x == p2.x && y == p2.y) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

// ============================================================
// Circle Drawing (Midpoint Circle Algorithm)
// ============================================================

void DrawCircle(uint8_t* fb, int width, const Point& center, int radius, Color color) {
    if (!fb || radius <= 0) return;

    int cx = center.x, cy = center.y;

    // Midpoint circle algorithm for filled circle
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x*x + y*y <= radius*radius) {
                set_pixel(fb, width, cx + x, cy + y, color);
            }
        }
    }
}

void DrawCircleBorder(uint8_t* fb, int width, const Point& center, int radius, int thickness, Color color) {
    if (!fb || radius <= 0) return;

    // Draw concentric circles for thickness
    for (int r = radius - thickness + 1; r <= radius; r++) {
        if (r <= 0) continue;

        int cx = center.x, cy = center.y;
        int x = r, y = 0;
        int err = 1 - r;

        while (x >= y) {
            set_pixel(fb, width, cx + x, cy + y, color);
            set_pixel(fb, width, cx + y, cy + x, color);
            set_pixel(fb, width, cx - y, cy + x, color);
            set_pixel(fb, width, cx - x, cy + y, color);
            set_pixel(fb, width, cx - x, cy - y, color);
            set_pixel(fb, width, cx - y, cy - x, color);
            set_pixel(fb, width, cx + y, cy - x, color);
            set_pixel(fb, width, cx + x, cy - y, color);

            y++;
            if (err < 0) {
                err += 2 * y + 1;
            } else {
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }
}

// ============================================================
// Rounded Rectangle Drawing
// ============================================================

static bool point_in_rounded_rect(int px, int py, const Rect& r, int radius) {
    if (px < r.x || py < r.y || px >= r.x + r.w || py >= r.y + r.h) {
        return false;
    }
    if (radius <= 0) {
        return true;
    }

    const int left = r.x + radius;
    const int right = r.x + r.w - 1 - radius;
    const int top = r.y + radius;
    const int bottom = r.y + r.h - 1 - radius;

    if ((px >= left && px <= right) || (py >= top && py <= bottom)) {
        return true;
    }

    const int cx = std::clamp(px, left, right);
    const int cy = std::clamp(py, top, bottom);
    const int dx = px - cx;
    const int dy = py - cy;
    return dx * dx + dy * dy <= radius * radius;
}

void DrawRoundRect(uint8_t* fb, int width, int height, const Rect& r, int radius,
                   Color fill_color, Color border_color, int border_thickness) {
    if (!fb || r.w <= 0 || r.h <= 0) return;

    // Clamp radius to half of min(w, h)
    int max_radius = std::min(r.w, r.h) / 2;
    radius = std::min(radius, max_radius);
    if (radius < 0) radius = 0;

    // Default height if not provided (for backward compatibility)
    if (height <= 0) height = 300;
    SetFramebufferHeightHint(height);

    Rect clipped = clamp_rect(r, width, height);
    for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
        for (int x = clipped.x; x < clipped.x + clipped.w; ++x) {
            if (point_in_rounded_rect(x, y, r, radius)) {
                set_pixel(fb, width, x, y, fill_color);
            }
        }
    }

    // Draw border if thickness > 0
    if (border_thickness > 0) {
        DrawRoundRectBorder(fb, width, height, r, radius, border_thickness, border_color);
    }
}

void DrawRoundRectBorder(uint8_t* fb, int width, int height, const Rect& r, int radius, int thickness, Color color) {
    if (!fb || r.w <= 0 || r.h <= 0 || thickness <= 0) return;

    int max_radius = std::min(r.w, r.h) / 2;
    radius = std::min(radius, max_radius);
    if (radius < 0) radius = 0;

    // Default height if not provided
    if (height <= 0) height = 300;
    SetFramebufferHeightHint(height);

    Rect clipped = clamp_rect(r, width, height);
    Rect inner = {r.x + thickness, r.y + thickness, r.w - thickness * 2, r.h - thickness * 2};
    int inner_radius = std::max(0, radius - thickness);

    for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
        for (int x = clipped.x; x < clipped.x + clipped.w; ++x) {
            if (!point_in_rounded_rect(x, y, r, radius)) continue;
            if (inner.w > 0 && inner.h > 0 && point_in_rounded_rect(x, y, inner, inner_radius)) continue;
            set_pixel(fb, width, x, y, color);
        }
    }
}

// Backward compatible versions (without height parameter)
void DrawRoundRect(uint8_t* fb, int width, const Rect& r, int radius,
                   Color fill_color, Color border_color, int border_thickness) {
    DrawRoundRect(fb, width, 300, r, radius, fill_color, border_color, border_thickness);
}

void DrawRoundRectBorder(uint8_t* fb, int width, const Rect& r, int radius, int thickness, Color color) {
    DrawRoundRectBorder(fb, width, 300, r, radius, thickness, color);
}

// ============================================================
// Text Rendering (from custom_lcd_display.cc:1292-1347)
// ============================================================

void DrawText(uint8_t* fb, int width, int x, int y, const char* text,
              const lv_font_t* font, Color color, int height) {
    if (!fb || !text || !font) return;
    SetFramebufferHeightHint(height);

    int cursor_x = x;
    int cursor_y = y;
    const char* p = text;

    while (*p) {
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        // Handle newline
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += font->line_height;
            continue;
        }

        // Get glyph descriptor (LVGL v9 API)
        // NOTE: lv_font_get_glyph_dsc_fmt_txt does NOT set resolved_font,
        // so we must set it ourselves before calling.
        lv_font_glyph_dsc_t g = {};
        g.resolved_font = font;
        if (!lv_font_get_glyph_dsc(font, &g, ch, 0)) {
            // Glyph not found, skip
            cursor_x += font->line_height / 2;
            continue;
        }

        // Get raw bitmap: v9 get_glyph_bitmap takes (g_dsc, draw_buf)
        // For A1 fonts, pass NULL draw_buf and set req_raw_bitmap=1 to get raw data
        g.req_raw_bitmap = 1;
        const uint8_t* bitmap = (const uint8_t*)lv_font_get_glyph_bitmap(&g, NULL);
        g.req_raw_bitmap = 0;

        if (!bitmap) {
            cursor_x += g.adv_w;
            continue;
        }

        // Calculate glyph position in framebuffer
        int gx = cursor_x + g.ofs_x;
        int gy = cursor_y + font->line_height - font->base_line - g.ofs_y - g.box_h;

        // Determine bitmap stride (bits per row)
        int row_bits = (g.stride > 0) ? (int)(g.stride * 8) : (int)g.box_w;

        // Copy glyph bitmap to framebuffer
        for (int row = 0; row < (int)g.box_h; row++) {
            for (int col = 0; col < (int)g.box_w; col++) {
                int bit_idx = row * row_bits + col;
                bool pixel = (bitmap[bit_idx >> 3] >> (7 - (bit_idx & 7))) & 1;

                if (pixel) {
                    int px = gx + col;
                    int py = gy + row;
                    if (px >= 0 && px < width && py >= 0 && py < height) {
                        set_pixel(fb, width, px, py, color);
                    }
                }
            }
        }

        cursor_x += g.adv_w;
    }
}

void DrawIcon(uint8_t* fb, int width, int x, int y, const char* icon_code,
              const lv_font_t* font, Color color) {
    DrawText(fb, width, x, y, icon_code, font, color);
}

// ============================================================
// Progress Bar
// ============================================================

void DrawProgress(uint8_t* fb, int width, const Rect& r, int value_pct,
                  Color bg_color, Color fg_color, int radius) {
    if (!fb || r.w <= 0 || r.h <= 0) return;

    // Clamp value
    value_pct = std::max(0, std::min(100, value_pct));

    // Default radius: pill shape (half height)
    if (radius < 0) {
        radius = r.h / 2;
    }

    // Clamp radius
    int max_radius = std::min(r.w, r.h) / 2;
    radius = std::min(radius, max_radius);

    // Draw background (full bar)
    DrawRoundRect(fb, width, r, radius, bg_color, bg_color, 0);

    // Draw foreground (filled portion)
    if (value_pct > 0) {
        int fg_w = (r.w * value_pct) / 100;
        if (fg_w > 0) {
            Rect fg_r = { r.x, r.y, fg_w, r.h };
            DrawRoundRect(fb, width, fg_r, radius, fg_color, fg_color, 0);
        }
    }
}

void DrawProgressWithLabel(uint8_t* fb, int width, int x, int y, int w, int h,
                           int value_pct, const char* label, const lv_font_t* font) {
    Rect r = { x, y, w, h };
    DrawProgress(fb, width, r, value_pct);

    // Draw label centered on bar
    if (label && font) {
        int text_w = MeasureTextWidth(label, font);
        int text_h = font->line_height;
        int label_x = x + (w - text_w) / 2;
        int label_y = y + (h - text_h) / 2;

        // Choose text color based on progress position
        int progress_x = x + (w * value_pct) / 100;
        Color text_color = (label_x < progress_x) ? WHITE : BLACK;

        DrawText(fb, width, label_x, label_y, label, font, text_color);
    }
}

// ============================================================
// Measurement Utilities
// ============================================================

int MeasureTextWidth(const char* text, const lv_font_t* font) {
    if (!text || !font) return 0;

    int width = 0;
    const char* p = text;

    while (*p) {
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        if (ch == '\n') {
            // Newline resets width for current line, but we return max width
            continue;
        }

        lv_font_glyph_dsc_t g = {};
        if (lv_font_get_glyph_dsc(font, &g, ch, 0)) {
            width += g.adv_w;
        } else {
            width += font->line_height / 2;  // Unknown char placeholder
        }
    }

    return width;
}

int MeasureTextHeight(const lv_font_t* font) {
    return font ? font->line_height : 16;
}

Rect MeasureTextBounds(const char* text, const lv_font_t* font, int max_width) {
    if (!text || !font) return { 0, 0, 0, 0 };

    int max_line_w = 0;
    int current_line_w = 0;
    int line_count = 1;
    const char* p = text;

    while (*p) {
        uint32_t ch = utf8_next(&p);
        if (ch == 0) break;

        if (ch == '\n') {
            max_line_w = std::max(max_line_w, current_line_w);
            current_line_w = 0;
            line_count++;
            continue;
        }

        lv_font_glyph_dsc_t g = {};
        int char_w = 0;
        if (lv_font_get_glyph_dsc(font, &g, ch, 0)) {
            char_w = g.adv_w;
        } else {
            char_w = font->line_height / 2;  // Unknown char placeholder
        }

        // Handle wrapping: if adding this char exceeds max_width, wrap to new line
        if (max_width > 0 && current_line_w + char_w > max_width && current_line_w > 0) {
            max_line_w = std::max(max_line_w, current_line_w);
            current_line_w = char_w;  // Start new line with this character
            line_count++;
        } else {
            current_line_w += char_w;
        }
    }

    max_line_w = std::max(max_line_w, current_line_w);

    return { 0, 0, max_line_w, line_count * font->line_height };
}

// ============================================================
// Region Operations
// ============================================================

void FillRect(uint8_t* fb, int width, const Rect& r, Color color) {
    DrawRect(fb, width, r, color);
}

void InvertRegion(uint8_t* fb, int width, const Rect& r) {
    if (!fb || r.w <= 0 || r.h <= 0) return;
    const int fb_height = g_framebuffer_height_hint.load(std::memory_order_relaxed);
    Rect clipped = clamp_rect(r, width, fb_height);
    if (clipped.w <= 0 || clipped.h <= 0) return;

    for (int y = clipped.y; y < clipped.y + clipped.h; y++) {
        for (int x = clipped.x; x < clipped.x + clipped.w; x++) {
            Color pixel = get_pixel_2bpp(fb, width, x, y);
            if (pixel == BLACK) {
                set_pixel_2bpp(fb, width, x, y, WHITE);
            } else if (pixel == WHITE) {
                set_pixel_2bpp(fb, width, x, y, BLACK);
            }
        }
    }
}

void CopyRegion(const uint8_t* src, uint8_t* dst, int width,
                const Rect& src_r, int dst_x, int dst_y) {
    if (!src || !dst) return;
    const int fb_height = g_framebuffer_height_hint.load(std::memory_order_relaxed);
    Rect clipped = clamp_rect(src_r, width, fb_height);
    if (clipped.w <= 0 || clipped.h <= 0) return;

    // Simple pixel-by-pixel copy
    for (int y = 0; y < clipped.h; y++) {
        for (int x = 0; x < clipped.w; x++) {
            Color pixel = get_pixel(src, width, clipped.x + x, clipped.y + y);
            set_pixel(dst, width, dst_x + x, dst_y + y, pixel);
        }
    }
}

void Clear(uint8_t* fb, int width, int height, Color fill) {
    if (!fb) return;
    SetFramebufferHeightHint(height);

    size_t bytes_per_row = (width * 2 + 7) >> 3;
    size_t total = bytes_per_row * height;
    memset(fb, ColorToFillByte(fill), total);
}

}  // namespace rawdraw
