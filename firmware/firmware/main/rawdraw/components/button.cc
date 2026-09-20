/**
 * @file button.cc
 * @brief Button implementation
 */

#include "button.h"
#include "rawdraw/theme.h"
#include <algorithm>

namespace rawdraw {

Button::Button(int x, int y, int w, int h, const char* icon_code, const lv_font_t* icon_font)
    : x_(x)
    , y_(y)
    , w_(w)
    , h_(h)
    , radius_(4)
    , icon_code_(icon_code)
    , icon_font_(icon_font)
    , text_(nullptr)
    , text_font_(nullptr)
    , pressed_(false)
    , callback_(nullptr)
    , bg_color_(WHITE)
    , fg_color_(BLACK)
    , border_color_(BLACK) {
}

Button::~Button() {}

void Button::SetPosition(int x, int y) { x_ = x; y_ = y; }
void Button::SetSize(int w, int h) { w_ = w; h_ = h; }
void Button::SetIcon(const char* icon_code) { icon_code_ = icon_code; }
void Button::SetIconFont(const lv_font_t* font) { icon_font_ = font; }
void Button::SetText(const char* text) { text_ = text; }
void Button::SetTextFont(const lv_font_t* font) { text_font_ = font; }
void Button::SetRadius(int radius) { radius_ = radius; }
void Button::SetCallback(ButtonCallback callback) { callback_ = callback; }

bool Button::Contains(int px, int py) const {
    return px >= x_ && px < x_ + w_ && py >= y_ && py < y_ + h_;
}

void Button::SetPressed(bool pressed) { pressed_ = pressed; }
bool Button::IsPressed() const { return pressed_; }

void Button::HandlePress() {
    SetPressed(true);
    if (callback_) {
        callback_();
    }
}

void Button::SetColors(Color bg, Color fg, Color border) {
    bg_color_ = bg;
    fg_color_ = fg;
    border_color_ = border;
}

Rect Button::GetBounds() const {
    return {x_, y_, w_, h_};
}

void Button::Draw(uint8_t* fb, int width, int height) {
    if (!fb) return;

    Rect bounds = GetBounds();
    bounds = clamp_rect(bounds, width, height);
    if (rect_area(bounds) <= 0) return;

    PaintStyle style = ThemeManager::Get().Component(pressed_ ? ComponentRole::ButtonSelected
                                                              : ComponentRole::ButtonNormal);
    if (bg_color_ != WHITE || fg_color_ != BLACK || border_color_ != BLACK) {
        style.bg = pressed_ ? fg_color_ : bg_color_;
        style.fg = pressed_ ? bg_color_ : fg_color_;
        style.border = pressed_ ? fg_color_ : border_color_;
    }

    // Draw button background
    DrawStyledRoundRect(fb, width, height, bounds, radius_, style);

    // Draw icon centered
    if (icon_code_ && icon_font_) {
        int icon_size = icon_font_->line_height;
        int icon_x = x_ + (w_ - icon_size) / 2;
        int icon_y = y_ + (h_ - icon_size) / 2 - (text_ ? (text_font_ ? text_font_->line_height / 2 : 8) : 0);
        DrawStyledIcon(fb, width, icon_x, icon_y, icon_code_, icon_font_, style);
    }

    // Draw text below icon
    if (text_ && text_font_) {
        int text_w = MeasureTextWidth(text_, text_font_);
        int text_x = x_ + (w_ - text_w) / 2;
        int text_y = y_ + h_ - text_font_->line_height - 4;
        DrawStyledText(fb, width, text_x, text_y, text_, text_font_, style, height);
    }
}

void Button::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

}  // namespace rawdraw
