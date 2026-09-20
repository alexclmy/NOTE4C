/**
 * @file panel.cc
 * @brief Panel implementation
 */

#include "panel.h"
#include "rawdraw/theme.h"
#include <algorithm>

namespace rawdraw {

Panel::Panel(int x, int y, int w, int h, int radius)
    : bounds_{x, y, w, h}
    , radius_(radius)
    , title_(nullptr)
    , title_font_(nullptr)
    , title_height_(0)
    , padding_(4)
    , border_width_(1)
    , title_enabled_(true)
    , bg_color_(WHITE)
    , border_color_(BLACK)
    , title_bg_color_(WHITE)
    , title_text_color_(BLACK) {
}

Panel::~Panel() {}

void Panel::SetBounds(const Rect& r) { bounds_ = r; }
void Panel::SetBounds(int x, int y, int w, int h) { bounds_ = {x, y, w, h}; }
void Panel::SetRadius(int radius) { radius_ = radius; }
void Panel::SetTitle(const char* title) { title_ = title; }
void Panel::SetTitleFont(const lv_font_t* font) { title_font_ = font; }
void Panel::SetTitleHeight(int height) { title_height_ = height; }
void Panel::SetPadding(int padding) { padding_ = padding; }
void Panel::SetBorderWidth(int width) { border_width_ = width; }
void Panel::SetTitleEnabled(bool enabled) { title_enabled_ = enabled; }

Rect Panel::GetBounds() const { return bounds_; }

int Panel::CalculateTitleHeight() const {
    if (!title_enabled_ || !title_) return 0;
    if (title_height_ > 0) return title_height_;
    if (title_font_) return title_font_->line_height + padding_ * 2;
    return 20;  // Default title bar height
}

Rect Panel::GetTitleBounds() const {
    int th = CalculateTitleHeight();
    return {bounds_.x, bounds_.y, bounds_.w, th};
}

Rect Panel::GetContentBounds() const {
    int th = CalculateTitleHeight();
    return {bounds_.x + padding_, bounds_.y + th + padding_,
            bounds_.w - 2 * padding_, bounds_.h - th - 2 * padding_};
}

void Panel::SetColors(Color bg, Color border) {
    bg_color_ = bg;
    border_color_ = border;
}

void Panel::SetTitleColors(Color bg, Color text) {
    title_bg_color_ = bg;
    title_text_color_ = text;
}

void Panel::Draw(uint8_t* fb, int width, int height) {
    if (!fb || rect_area(bounds_) <= 0) return;

    Rect bounds = clamp_rect(bounds_, width, height);
    if (rect_area(bounds) <= 0) return;

    PaintStyle panel_style = ThemeManager::Get().Component(ComponentRole::Panel);
    if (bg_color_ != WHITE || border_color_ != BLACK) {
        panel_style.bg = bg_color_;
        panel_style.border = border_color_;
    }
    panel_style.border_width = border_width_;

    // Draw panel background with border
    DrawStyledRoundRect(fb, width, height, bounds, radius_, panel_style);

    // Draw title bar if enabled
    if (title_enabled_ && title_) {
        int th = CalculateTitleHeight();
        if (th > 0) {
            // Title bar background (top portion of panel)
            Rect title_bg = {bounds_.x, bounds_.y, bounds_.w, th};
            title_bg = clamp_rect(title_bg, width, height);

            // Fill title bar area (draw over panel bg)
            PaintStyle title_style = ThemeManager::Get().Style(ThemeToken::BackgroundSecondary);
            title_style.bg = title_bg_color_;
            title_style.fg = title_text_color_;
            DrawStyledRect(fb, width, title_bg, title_style);

            // Draw title text
            if (title_font_) {
                int text_x = bounds_.x + padding_;
                int text_y = bounds_.y + padding_;
                DrawStyledText(fb, width, text_x, text_y, title_, title_font_, title_style, height);
            }

            // Draw separator line below title bar
            if (th < bounds_.h) {
                int sep_y = bounds_.y + th;
                DrawHLine(fb, width, sep_y, bounds_.x, bounds_.x + bounds_.w - 1, panel_style.border);
            }
        }
    }
}

void Panel::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

}  // namespace rawdraw
