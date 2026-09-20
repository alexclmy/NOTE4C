/**
 * @file progress_bar.cc
 * @brief Progress bar implementation
 */

#include "progress_bar.h"
#include "rawdraw/theme.h"
#include <algorithm>
#include <cmath>

namespace rawdraw {

// ============================================================
// Circular Progress (standalone functions)
// ============================================================

/**
 * @brief Draw a ring arc (annular sector) at given radius and thickness
 *
 * Only draws pixels within the annular region (inner_r <= dist <= outer_r)
 * and within the angular span (start_deg <= angle <= end_deg).
 * Clock degrees: 0 = 12-o'clock, clockwise increases.
 */
static void draw_ring_arc(uint8_t* fb, int width, int cx, int cy, int outer_r, int thickness,
                          float start_deg, float end_deg, Color color) {
    int inner_r = outer_r - thickness + 1;
    if (inner_r < 1) inner_r = 1;

    const float PI = 3.14159265f;
    const float TWO_PI = 2.0f * PI;
    const float DEG_TO_RAD = PI / 180.0f;

    float start_math = (start_deg - 90.0f) * DEG_TO_RAD;
    float end_math = (end_deg - 90.0f) * DEG_TO_RAD;

    while (start_math < 0) start_math += TWO_PI;
    while (start_math >= TWO_PI) start_math -= TWO_PI;
    while (end_math < 0) end_math += TWO_PI;
    while (end_math >= TWO_PI) end_math -= TWO_PI;

    if (end_math <= start_math) {
        end_math += TWO_PI;
    }

    float arc_span = end_math - start_math;

    // Iterate bounding box and only draw pixels in the annular + angular region
    for (int dy = -outer_r; dy <= outer_r; dy++) {
        for (int dx = -outer_r; dx <= outer_r; dx++) {
            int dist_sq = dx * dx + dy * dy;
            // Annular check: between inner and outer radius
            if (dist_sq > outer_r * outer_r) continue;
            if (dist_sq < inner_r * inner_r) continue;

            float angle = atan2f((float)dy, (float)dx);
            if (angle < 0) angle += TWO_PI;

            float delta = angle - start_math;
            if (delta < 0) delta += TWO_PI;

            if (delta <= arc_span) {
                set_pixel(fb, width, cx + dx, cy + dy, color);
            }
        }
    }
}

void DrawCircularProgress(uint8_t* fb, int width, const Point& center, int radius,
                          int thickness, int value_pct,
                          Color bg_color, Color fg_color) {
    if (!fb || radius <= 0 || thickness <= 0) return;

    value_pct = std::max(0, std::min(100, value_pct));

    // Draw background ring (full 360°)
    draw_ring_arc(fb, width, center.x, center.y, radius, thickness, 0.0f, 360.0f, bg_color);

    // Draw foreground arc (value percentage, clockwise from 12-o'clock)
    if (value_pct > 0) {
        float end_deg = (value_pct * 360.0f) / 100.0f;
        draw_ring_arc(fb, width, center.x, center.y, radius, thickness, 0.0f, end_deg, fg_color);
    }
}

void DrawCircularProgressWithLabel(uint8_t* fb, int width, const Point& center, int radius,
                                   int thickness, int value_pct,
                                   const char* label, const lv_font_t* font) {
    DrawCircularProgress(fb, width, center, radius, thickness, value_pct);

    if (label && font) {
        int text_w = MeasureTextWidth(label, font);
        int text_h = font->line_height;
        int label_x = center.x - text_w / 2;
        int label_y = center.y - text_h / 2;
        DrawText(fb, width, label_x, label_y, label, font,
                 ThemeManager::Get().ColorFor(ThemeToken::TextPrimary));
    }
}

// ============================================================
// Horizontal Progress Bar
// ============================================================

ProgressBar::ProgressBar(int x, int y, int w, int h)
    : bounds_{x, y, w, h}
    , value_(0)
    , radius_(h / 2)  // Default: pill shape
    , label_(nullptr)
    , label_font_(nullptr)
    , bg_color_(WHITE)
    , fg_color_(BLACK) {
}

ProgressBar::~ProgressBar() {}

void ProgressBar::SetBounds(const Rect& r) { bounds_ = r; }
void ProgressBar::SetBounds(int x, int y, int w, int h) { bounds_ = {x, y, w, h}; }

void ProgressBar::SetValue(int value) {
    value_ = std::max(0, std::min(100, value));
}

int ProgressBar::GetValue() const { return value_; }

void ProgressBar::SetLabel(const char* label) { label_ = label; }
void ProgressBar::SetLabelFont(const lv_font_t* font) { label_font_ = font; }
void ProgressBar::SetRadius(int radius) { radius_ = radius; }

Rect ProgressBar::GetBounds() const { return bounds_; }

void ProgressBar::SetBgColor(Color color) { bg_color_ = color; }
void ProgressBar::SetFgColor(Color color) { fg_color_ = color; }

void ProgressBar::Draw(uint8_t* fb, int width, int height) {
    if (!fb || rect_area(bounds_) <= 0) return;

    Rect bounds = clamp_rect(bounds_, width, height);
    if (rect_area(bounds) <= 0) return;

    // Clamp radius
    int max_radius = std::min(bounds.w, bounds.h) / 2;
    int r = std::min(radius_, max_radius);

    PaintStyle style = ThemeManager::Get().Component(ComponentRole::Progress);
    if (bg_color_ != WHITE || fg_color_ != BLACK) {
        style.bg = bg_color_;
        style.fg = fg_color_;
    }
    DrawStyledProgress(fb, width, bounds, value_, style, r);

    // Draw label if set
    if (label_ && label_font_) {
        int text_w = MeasureTextWidth(label_, label_font_);
        int text_h = label_font_->line_height;
        int label_x = bounds.x + (bounds.w - text_w) / 2;
        int label_y = bounds.y + (bounds.h - text_h) / 2;

        // Choose color based on position relative to fill
        int fill_x = bounds.x + (bounds.w * value_) / 100;
        Color text_color = (label_x < fill_x) ? style.bg : style.fg;

        DrawText(fb, width, label_x, label_y, label_, label_font_, text_color);
    }
}

void ProgressBar::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

// ============================================================
// Circular Gauge Component
// ============================================================

CircularGauge::CircularGauge(int cx, int cy, int radius, int thickness)
    : cx_(cx)
    , cy_(cy)
    , radius_(radius)
    , thickness_(thickness)
    , value_(0)
    , label_(nullptr)
    , label_font_(nullptr)
    , bg_color_(WHITE)
    , fg_color_(BLACK) {
}

CircularGauge::~CircularGauge() {}

void CircularGauge::SetCenter(int cx, int cy) { cx_ = cx; cy_ = cy; }
void CircularGauge::SetRadius(int radius) { radius_ = radius; }
void CircularGauge::SetThickness(int thickness) { thickness_ = thickness; }

void CircularGauge::SetValue(int value) {
    value_ = std::max(0, std::min(100, value));
}

int CircularGauge::GetValue() const { return value_; }

void CircularGauge::SetLabel(const char* label) { label_ = label; }
void CircularGauge::SetLabelFont(const lv_font_t* font) { label_font_ = font; }

Rect CircularGauge::GetBounds() const {
    return { cx_ - radius_, cy_ - radius_, radius_ * 2, radius_ * 2 };
}

void CircularGauge::SetBgColor(Color color) { bg_color_ = color; }
void CircularGauge::SetFgColor(Color color) { fg_color_ = color; }

void CircularGauge::Draw(uint8_t* fb, int width, int height) {
    if (!fb || radius_ <= 0) return;

    Point center = { cx_, cy_ };
    Color bg = bg_color_;
    Color fg = fg_color_;
    if (bg_color_ == WHITE && fg_color_ == BLACK) {
        const PaintStyle style = ThemeManager::Get().Component(ComponentRole::Progress);
        bg = style.bg;
        fg = style.fg;
    }
    DrawCircularProgress(fb, width, center, radius_, thickness_, value_, bg, fg);

    // Draw center label
    if (label_ && label_font_) {
        int text_w = MeasureTextWidth(label_, label_font_);
        int text_h = label_font_->line_height;
        int label_x = cx_ - text_w / 2;
        int label_y = cy_ - text_h / 2;
        DrawText(fb, width, label_x, label_y, label_, label_font_,
                 ThemeManager::Get().ColorFor(ThemeToken::TextPrimary));
    }
}

void CircularGauge::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

}  // namespace rawdraw
