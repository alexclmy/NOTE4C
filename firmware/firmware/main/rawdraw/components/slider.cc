/**
 * @file slider.cc
 * @brief Horizontal slider implementation
 */

#include "slider.h"
#include "rawdraw/theme.h"
#include "style.h"
#include <algorithm>
#include <cstdio>

namespace rawdraw {

Slider::Slider(int x, int y, int w, int h, int min_val, int max_val)
    : x_(x)
    , y_(y)
    , w_(w)
    , h_(h)
    , min_val_(min_val)
    , max_val_(max_val)
    , value_(min_val)
    , min_label_(nullptr)
    , max_label_(nullptr)
    , value_label_(nullptr)
    , font_(nullptr)
    , callback_(nullptr)
    , track_bg_color_(WHITE)
    , track_fill_color_(BLACK)
    , thumb_color_(BLACK)
    , text_color_(BLACK)
    , border_color_(BLACK) {
    // Set default min/max labels
    snprintf(min_label_buf_, sizeof(min_label_buf_), "%d", min_val_);
    snprintf(max_label_buf_, sizeof(max_label_buf_), "%d", max_val_);
}

Slider::~Slider() {}

void Slider::SetPosition(int x, int y) { x_ = x; y_ = y; }
void Slider::SetSize(int w, int h) { w_ = w; h_ = h; }

void Slider::SetRange(int min_val, int max_val) {
    min_val_ = min_val;
    max_val_ = max_val;
    if (value_ < min_val_) value_ = min_val_;
    if (value_ > max_val_) value_ = max_val_;
}

void Slider::SetValue(int value) {
    value_ = std::max(min_val_, std::min(max_val_, value));
}

int Slider::GetValue() const { return value_; }

int Slider::GetValuePercent() const {
    if (max_val_ == min_val_) return 0;
    return ((value_ - min_val_) * 100) / (max_val_ - min_val_);
}

void Slider::SetLabels(const char* min_label, const char* max_label, const char* value_label) {
    min_label_ = min_label;
    max_label_ = max_label;
    value_label_ = value_label;
}

void Slider::SetFont(const lv_font_t* font) { font_ = font; }
void Slider::SetCallback(SliderCallback callback) { callback_ = callback; }

bool Slider::Contains(int px, int py) const {
    Rect b = GetBounds();
    return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
}

bool Slider::HandleDrag(int px) {
    int old_value = value_;
    value_ = XToValue(px);
    value_ = std::max(min_val_, std::min(max_val_, value_));
    if (value_ != old_value && callback_) {
        callback_(value_);
        return true;
    }
    return value_ != old_value;
}

Rect Slider::GetBounds() const {
    return {x_, y_, w_, h_};
}

Rect Slider::GetTrackBounds() const {
    // Track is the horizontal bar portion (middle vertical section)
    int track_h = std::max(4, h_ / 3);
    int track_y = y_ + (h_ - track_h) / 2;
    return {x_, track_y, w_, track_h};
}

Point Slider::GetThumbCenter() const {
    int pct = GetValuePercent();
    int thumb_x = x_ + (w_ * pct) / 100;
    int thumb_y = y_ + h_ / 2;
    return {thumb_x, thumb_y};
}

int Slider::XToValue(int px) const {
    if (w_ <= 0) return min_val_;
    int clamped_x = std::max(x_, std::min(x_ + w_, px));
    int pct = ((clamped_x - x_) * 100) / w_;
    return min_val_ + (pct * (max_val_ - min_val_)) / 100;
}

void Slider::SetColors(Color track_bg, Color track_fill, Color thumb, Color text) {
    track_bg_color_ = track_bg;
    track_fill_color_ = track_fill;
    thumb_color_ = thumb;
    text_color_ = text;
}

void Slider::Draw(uint8_t* fb, int width, int height) {
    if (!fb || w_ <= 0 || h_ <= 0) return;

    Rect bounds = clamp_rect({x_, y_, w_, h_}, width, height);
    if (rect_area(bounds) <= 0) return;

    Rect track = GetTrackBounds();
    track = clamp_rect(track, width, height);

    int track_radius = track.h / 2;  // Pill shape
    if (track_radius < 1) track_radius = 1;
    int max_r = std::min(track.w, track.h) / 2;
    if (track_radius > max_r) track_radius = max_r;

    const auto& theme = ThemeManager::Get();
    const bool default_colors = track_bg_color_ == WHITE && track_fill_color_ == BLACK &&
                                thumb_color_ == BLACK && text_color_ == BLACK &&
                                border_color_ == BLACK;
    PaintStyle progress_style = theme.Component(ComponentRole::Progress);
    Color track_bg = track_bg_color_;
    Color track_fill = track_fill_color_;
    Color thumb_fill = thumb_color_;
    Color text_color = text_color_;
    Color border_color = border_color_;
    DitherToken track_dither = DitherToken::None;
    if (default_colors) {
        track_bg = progress_style.bg;
        track_fill = progress_style.fg;
        thumb_fill = progress_style.border;
        text_color = theme.ColorFor(ThemeToken::TextSecondary);
        border_color = progress_style.border;
        track_dither = progress_style.dither;
    }

    // Draw track background
    DrawStyledRoundRect(fb, width, height, track, track_radius,
                        MakePaint(text_color, track_bg, border_color, track_dither,
                                  1, RefreshCost::SmallAccent));

    // Draw filled portion of track
    int pct = GetValuePercent();
    if (pct > 0) {
        int fill_w = (track.w * pct) / 100;
        if (fill_w > 0) {
            Rect fill_rect = {track.x, track.y, fill_w, track.h};
            // For 1bpp: fill with black (filled portion)
            DrawRoundRect(fb, width, fill_rect, track_radius,
                          track_fill, track_fill, 0);
        }
    }

    // Draw track border outline
    DrawRoundRectBorder(fb, width, track, track_radius, 1, border_color);

    // Draw thumb (diamond shape for 1bpp ePaper - stands out well)
    Point thumb = GetThumbCenter();
    int thumb_size = std::max(4, track.h / 2 + 2);

    // Diamond shape using lines (better visibility on 1bpp than circle)
    Point top    = {thumb.x, thumb.y - thumb_size};
    Point bottom = {thumb.x, thumb.y + thumb_size};
    Point left   = {thumb.x - thumb_size, thumb.y};
    Point right  = {thumb.x + thumb_size, thumb.y};

    // Fill diamond: draw horizontal lines from top to bottom
    for (int dy = -thumb_size; dy <= thumb_size; dy++) {
        int half_w = thumb_size - abs(dy);
        if (half_w <= 0) continue;
        int ly = thumb.y + dy;
        if (ly < 0 || ly >= height) continue;
        DrawHLine(fb, width, ly, thumb.x - half_w, thumb.x + half_w, thumb_fill);
    }

    // Draw min label (use buffer if not explicitly set)
    const char* min_text = min_label_ ? min_label_ : min_label_buf_;
    if (min_text && font_) {
        int text_x = x_;
        int text_y = y_ - font_->line_height - Style::kSpacingXS;
        if (text_y < 0) text_y = y_ + h_ + Style::kSpacingXS;
        DrawText(fb, width, text_x, text_y, min_text, font_, text_color);
    }

    // Draw max label (use buffer if not explicitly set)
    const char* max_text = max_label_ ? max_label_ : max_label_buf_;
    if (max_text && font_) {
        int text_w = MeasureTextWidth(max_text, font_);
        int text_x = x_ + w_ - text_w;
        int text_y = y_ - font_->line_height - Style::kSpacingXS;
        if (text_y < 0) text_y = y_ + h_ + Style::kSpacingXS;
        DrawText(fb, width, text_x, text_y, max_text, font_, text_color);
    }

    // Draw value label above thumb
    if (value_label_ && font_) {
        int text_w = MeasureTextWidth(value_label_, font_);
        int text_x = thumb.x - text_w / 2;
        int text_y = thumb.y - thumb_size - font_->line_height - Style::kSpacingXS;
        if (text_y < 0) text_y = thumb.y + thumb_size + Style::kSpacingXS;
        DrawText(fb, width, text_x, text_y, value_label_, font_, text_color);
    }
}

void Slider::Draw(Framebuffer* fb, int screen_width, int screen_height) {
    if (!fb) return;
    fb->SafeDraw([this, screen_width, screen_height](uint8_t* buffer) {
        this->Draw(buffer, screen_width, screen_height);
    });
}

}  // namespace rawdraw
