/**
 * @file card.cc
 * @brief Card container implementation
 */

#include "card.h"
#include "style.h"
#include "rawdraw/theme.h"
#include <algorithm>

namespace rawdraw {

Card::Card(int x, int y, int w, int h, int radius)
    : bounds_{x, y, w, h}
    , radius_(radius)
    , border_width_(Style::kBorderThin)
    , padding_(Style::kSpacingSM)
    , title_(nullptr)
    , title_font_(nullptr)
    , title_height_(0)
    , title_enabled_(true)
    , shadow_enabled_(false)
    , shadow_offset_(2)
    , bg_color_(WHITE)
    , border_color_(BLACK)
    , title_bg_color_(WHITE)
    , title_text_color_(BLACK)
    , shadow_color_(BLACK) {
}

Card::~Card() {}

void Card::SetBounds(const Rect& r) { bounds_ = r; }
void Card::SetBounds(int x, int y, int w, int h) { bounds_ = {x, y, w, h}; }
void Card::SetRadius(int radius) { radius_ = radius; }
void Card::SetBorderWidth(int width) { border_width_ = width; }
void Card::SetPadding(int padding) { padding_ = padding; }
void Card::SetTitle(const char* title) { title_ = title; }
void Card::SetTitleFont(const lv_font_t* font) { title_font_ = font; }
void Card::SetTitleHeight(int height) { title_height_ = height; }
void Card::SetTitleEnabled(bool enabled) { title_enabled_ = enabled; }
void Card::SetShadowEnabled(bool enabled) { shadow_enabled_ = enabled; }
void Card::SetShadowOffset(int offset) { shadow_offset_ = offset; }

Rect Card::GetBounds() const { return bounds_; }

int Card::CalculateTitleHeight() const {
    if (!title_enabled_ || !title_) return 0;
    if (title_height_ > 0) return title_height_;
    if (title_font_) return title_font_->line_height + padding_ * 2;
    return Style::kPanelTitleHeight;
}

Rect Card::GetTitleBounds() const {
    int th = CalculateTitleHeight();
    return {bounds_.x, bounds_.y, bounds_.w, th};
}

Rect Card::GetContentBounds() const {
    int th = CalculateTitleHeight();
    return {bounds_.x + padding_, bounds_.y + th + padding_,
            std::max(0, bounds_.w - 2 * padding_),
            std::max(0, bounds_.h - th - 2 * padding_)};
}

void Card::SetColors(Color bg, Color border) {
    bg_color_ = bg;
    border_color_ = border;
}

void Card::SetTitleColors(Color bg, Color text) {
    title_bg_color_ = bg;
    title_text_color_ = text;
}

void Card::SetShadowColor(Color color) {
    shadow_color_ = color;
}

void Card::Draw(uint8_t* fb, int width, int height) {
    if (!fb || rect_area(bounds_) <= 0) return;

    Rect bounds = clamp_rect(bounds_, width, height);
    if (rect_area(bounds) <= 0) return;

    // Clamp radius to valid range
    int r = std::min(radius_, std::min(bounds.w, bounds.h) / 2);
    if (r < 0) r = 0;

    PaintStyle card_style = ThemeManager::Get().Component(ComponentRole::CardDefault);
    if (bg_color_ != WHITE || border_color_ != BLACK) {
        card_style.bg = bg_color_;
        card_style.border = border_color_;
    }
    PaintStyle shadow_style = ThemeManager::Get().Style(ThemeToken::Shadow);
    shadow_style.bg = shadow_color_;

    // 1. Draw shadow (offset rect behind card)
    if (shadow_enabled_ && shadow_offset_ > 0) {
        Rect shadow_rect = {
            bounds.x + shadow_offset_,
            bounds.y + shadow_offset_,
            bounds.w,
            bounds.h
        };
        shadow_rect = clamp_rect(shadow_rect, width, height);
        if (rect_area(shadow_rect) > 0) {
            // Draw shadow as a simple filled rect (no rounded corners needed for shadow)
            DrawStyledRect(fb, width, shadow_rect, shadow_style);
        }
    }

    // 2. Draw card background with border
    card_style.border_width = border_width_;
    DrawStyledRoundRect(fb, width, height, bounds, r, card_style);

    // 3. Draw title bar if enabled
    if (title_enabled_ && title_) {
        int th = CalculateTitleHeight();
        if (th > 0) {
            Rect title_bg = {bounds.x, bounds.y, bounds.w, th};
            title_bg = clamp_rect(title_bg, width, height);

            // Fill title bar area with contrasting background
            // For 1bpp: title bar gets black fill with white text for contrast
            PaintStyle title_style = ThemeManager::Get().Style(ThemeToken::BackgroundSecondary);
            title_style.bg = title_bg_color_;
            title_style.fg = title_text_color_;
            DrawStyledRect(fb, width, title_bg, title_style);

            // Draw title text
            if (title_font_) {
                int text_x = bounds.x + padding_;
                int text_y = bounds.y + padding_;
                DrawStyledText(fb, width, text_x, text_y, title_, title_font_, title_style, height);
            }

            // Draw separator line below title bar
            if (th < bounds.h) {
                int sep_y = bounds.y + th;
                DrawHLine(fb, width, sep_y, bounds.x, bounds.x + bounds.w - 1, card_style.border);
            }
        }
    }
}

void Card::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

}  // namespace rawdraw
