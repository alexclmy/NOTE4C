/**
 * @file toggle.cc
 * @brief Toggle switch implementation
 */

#include "toggle.h"
#include "rawdraw/theme.h"
#include "style.h"
#include <algorithm>

namespace rawdraw {

Toggle::Toggle(int x, int y, int w, int h)
    : x_(x)
    , y_(y)
    , w_(w)
    , h_(h)
    , state_(false)
    , label_(nullptr)
    , font_(nullptr)
    , callback_(nullptr)
    , track_on_color_(BLACK)
    , track_off_color_(WHITE)
    , thumb_color_(WHITE)
    , border_color_(BLACK) {
}

Toggle::~Toggle() {}

void Toggle::SetPosition(int x, int y) { x_ = x; y_ = y; }
void Toggle::SetSize(int w, int h) { w_ = w; h_ = h; }

void Toggle::SetState(bool on) { state_ = on; }
bool Toggle::GetState() const { return state_; }

void Toggle::SetLabel(const char* label) { label_ = label; }
void Toggle::SetFont(const lv_font_t* font) { font_ = font; }
void Toggle::SetCallback(ToggleCallback callback) { callback_ = callback; }

bool Toggle::Contains(int px, int py) const {
    Rect b = GetBounds();
    return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
}

void Toggle::HandleTap() {
    state_ = !state_;
    if (callback_) {
        callback_(state_);
    }
}

Rect Toggle::GetTrackBounds() const {
    return {x_, y_, w_, h_};
}

Rect Toggle::GetBounds(int screen_width) const {
    int total_w = w_;
    if (label_ && font_) {
        total_w = w_ + Style::kSpacingSM + MeasureTextWidth(label_, font_);
    }
    return {x_, y_, total_w, h_};
}

Point Toggle::GetThumbCenter() const {
    int radius = h_ / 2;
    int padding = 2;
    int thumb_x = state_
        ? (x_ + w_ - radius - padding)
        : (x_ + radius + padding);
    int thumb_y = y_ + h_ / 2;
    return {thumb_x, thumb_y};
}

void Toggle::SetColors(Color track_on, Color track_off, Color thumb, Color border) {
    track_on_color_ = track_on;
    track_off_color_ = track_off;
    thumb_color_ = thumb;
    border_color_ = border;
}

void Toggle::Draw(uint8_t* fb, int width, int height) {
    if (!fb || w_ <= 0 || h_ <= 0) return;

    Rect track = clamp_rect({x_, y_, w_, h_}, width, height);
    if (rect_area(track) <= 0) return;

    int radius = h_ / 2;  // Pill shape
    if (radius < 1) radius = 1;

    const auto& theme = ThemeManager::Get();
    const PaintStyle on_style = theme.Component(ComponentRole::SettingsSelected);
    const PaintStyle off_style = theme.Component(ComponentRole::SettingsRow);
    const PaintStyle disabled_style = theme.Style(ThemeToken::Disabled);
    const bool default_colors = track_on_color_ == BLACK && track_off_color_ == WHITE &&
                                thumb_color_ == WHITE && border_color_ == BLACK;

    Color track_fill = state_ ? track_on_color_ : track_off_color_;
    Color track_border = border_color_;
    Color thumb_fill = state_ ? thumb_color_ : BLACK;
    Color thumb_inner = WHITE;
    Color label_color = BLACK;
    DitherToken track_dither = DitherToken::None;
    if (default_colors) {
        const PaintStyle track_style = state_ ? on_style : off_style;
        track_fill = track_style.bg;
        track_border = track_style.border;
        track_dither = state_ ? DitherToken::None : disabled_style.dither;
        thumb_fill = state_ ? track_style.fg : track_style.border;
        thumb_inner = state_ ? theme.ColorFor(ThemeToken::BackgroundPrimary) : track_style.bg;
        label_color = theme.ColorFor(ThemeToken::TextPrimary);
    }

    // Draw track background (pill shape)
    PaintStyle track_style = MakePaint(track_fill, track_fill, track_border,
                                       track_dither, 1,
                                       RefreshCost::SmallAccent);
    DrawStyledRoundRect(fb, width, height, track, radius, track_style);

    // Draw thumb
    Point thumb = GetThumbCenter();
    int thumb_r = radius - 2;
    if (thumb_r < 1) thumb_r = 1;

    if (state_) {
        // ON: filled white circle on black track
        DrawCircle(fb, width, thumb, thumb_r, thumb_fill);
    } else {
        // OFF: outlined circle on the themed track
        DrawCircle(fb, width, thumb, thumb_r, thumb_fill);
        if (thumb_r > 2) {
            DrawCircle(fb, width, thumb, thumb_r - 2, thumb_inner);
        }
    }

    // Draw label to the right of toggle
    if (label_ && font_) {
        int text_x = x_ + w_ + Style::kSpacingSM;
        int text_y = y_ + (h_ - font_->line_height) / 2;
        DrawText(fb, width, text_x, text_y, label_, font_, label_color);
    }
}

void Toggle::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

}  // namespace rawdraw
